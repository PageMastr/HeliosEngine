package rpc_test

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/propagation"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
	"go.opentelemetry.io/otel/trace"

	"github.com/PageMastr/scifi-test/services/pkg/rpc"
	"github.com/PageMastr/scifi-test/services/pkg/testkit"
)

type echoReq struct {
	Name string `json:"name"`
	ID   int64  `json:"id,string"`
}
type echoRes struct {
	Greeting string `json:"greeting"`
}

func echo(_ context.Context, _ *http.Request, req *echoReq) (*echoRes, error) {
	switch req.Name {
	case "":
		return nil, rpc.Errorf(rpc.CodeInvalidArgument, "name is required")
	case "slow":
		return nil, &rpc.Error{Code: rpc.CodeResourceExhausted, Message: "slow down", RetryAfter: 1500 * time.Millisecond}
	case "boom":
		return nil, errors.New("database password is hunter2")
	}
	return &echoRes{Greeting: "hello " + req.Name}, nil
}

func post(t *testing.T, h http.Handler, method, ct, body string) (*http.Response, string) {
	t.Helper()
	r := httptest.NewRequest(method, "/x.v1.Echo/Say", strings.NewReader(body))
	if ct != "" {
		r.Header.Set("Content-Type", ct)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	b, _ := io.ReadAll(w.Result().Body)
	return w.Result(), string(b)
}

func TestUnaryHTTP(t *testing.T) {
	h := rpc.Unary(echo, rpc.UnaryOptions{MaxBodyBytes: 128})

	res, body := post(t, h, http.MethodPost, "application/json; charset=utf-8", `{"name":"ada","id":"9007199254740993","extra":1}`)
	if res.StatusCode != 200 || body != `{"greeting":"hello ada"}` {
		t.Fatalf("ok case: %d %s", res.StatusCode, body)
	}
	if res.Header.Get("Cache-Control") != "no-store" {
		t.Fatal("responses must not be cached")
	}

	cases := []struct {
		method, ct, body string
		status           int
		code             rpc.Code
	}{
		{http.MethodGet, "", "", 501, rpc.CodeUnimplemented},
		{http.MethodPost, "text/plain", `{}`, 400, rpc.CodeInvalidArgument},
		{http.MethodPost, "application/json", `{"name":`, 400, rpc.CodeInvalidArgument},
		{http.MethodPost, "application/json", `{}`, 400, rpc.CodeInvalidArgument},
		{http.MethodPost, "application/json", `{"name":"` + strings.Repeat("a", 200) + `"}`, 429, rpc.CodeResourceExhausted},
		{http.MethodPost, "application/json", `{"name":"slow"}`, 429, rpc.CodeResourceExhausted},
		{http.MethodPost, "application/json", `{"name":"boom"}`, 500, rpc.CodeInternal},
	}
	for _, c := range cases {
		res, body := post(t, h, c.method, c.ct, c.body)
		e := rpc.DecodeError(res.StatusCode, []byte(body))
		if res.StatusCode != c.status || e.Code != c.code {
			t.Errorf("%s %s %q: got %d %s", c.method, c.ct, c.body, res.StatusCode, body)
		}
		if strings.Contains(body, "hunter2") {
			t.Fatal("internal error details leaked to the client")
		}
		if c.body == `{"name":"slow"}` && res.Header.Get("Retry-After") != "2" {
			t.Errorf("Retry-After = %q, want 2 (rounded up)", res.Header.Get("Retry-After"))
		}
	}
}

func TestHTTPStatusMapping(t *testing.T) {
	want := map[rpc.Code]int{
		rpc.CodeUnauthenticated: 401, rpc.CodePermissionDenied: 403, rpc.CodeNotFound: 404,
		rpc.CodeAlreadyExists: 409, rpc.CodeUnavailable: 503, rpc.CodeDeadlineExceeded: 504,
		rpc.CodeFailedPrecondition: 400, rpc.CodeCanceled: 499, rpc.Code("weird"): 500,
	}
	for c, s := range want {
		if got := rpc.HTTPStatus(c); got != s {
			t.Errorf("%s -> %d, want %d", c, got, s)
		}
	}
	if rpc.CodeOf(nil) != "" || rpc.CodeOf(errors.New("x")) != rpc.CodeInternal {
		t.Fatal("CodeOf")
	}
	wrapped := rpc.Internal(errors.New("cause"))
	if !strings.Contains(wrapped.Error(), "cause") || wrapped.Unwrap() == nil {
		t.Fatal("Internal must keep the cause for logs")
	}
}

type natsReq struct {
	N int `json:"n"`
}
type natsRes struct {
	Double int `json:"double"`
}

func TestNATSRoundTrip(t *testing.T) {
	bus := testkit.StartNATS(t)
	sawDeadline := make(chan time.Duration, 1)
	_, err := rpc.NATSHandle(bus.Conn, "rpc.test.math.Double", "math", nil,
		func(ctx context.Context, req *natsReq) (*natsRes, error) {
			if dl, ok := ctx.Deadline(); ok {
				select {
				case sawDeadline <- time.Until(dl):
				default:
				}
			}
			if req.N < 0 {
				return nil, rpc.Errorf(rpc.CodeInvalidArgument, "negative")
			}
			return &natsRes{Double: 2 * req.N}, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	client := bus.Connect(t, "client")

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	res, err := rpc.NATSRequest[natsReq, natsRes](ctx, client, "rpc.test.math.Double", &natsReq{N: 21})
	if err != nil || res.Double != 42 {
		t.Fatalf("got %+v %v", res, err)
	}
	if d := <-sawDeadline; d <= 0 || d > 3*time.Second {
		t.Fatalf("handler deadline budget %v not propagated", d)
	}

	_, err = rpc.NATSRequest[natsReq, natsRes](ctx, client, "rpc.test.math.Double", &natsReq{N: -1})
	if rpc.CodeOf(err) != rpc.CodeInvalidArgument {
		t.Fatalf("error code not propagated: %v", err)
	}
	_, err = rpc.NATSRequest[natsReq, natsRes](ctx, client, "rpc.test.math.Nobody", &natsReq{N: 1})
	if rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("no responders should be unavailable: %v", err)
	}
	// Malformed request body.
	reply, err := client.Request("rpc.test.math.Double", []byte("{"), 2*time.Second)
	if err != nil || reply.Header.Get(rpc.HeaderErrorCode) != string(rpc.CodeInvalidArgument) {
		t.Fatalf("malformed body: %v %v", reply, err)
	}
}

func TestNATSTracePropagation(t *testing.T) {
	rec := tracetest.NewSpanRecorder()
	tp := sdktrace.NewTracerProvider(sdktrace.WithSpanProcessor(rec))
	prevTP, prevProp := otel.GetTracerProvider(), otel.GetTextMapPropagator()
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.TraceContext{})
	t.Cleanup(func() { otel.SetTracerProvider(prevTP); otel.SetTextMapPropagator(prevProp) })

	bus := testkit.StartNATS(t)
	if _, err := rpc.NATSHandle(bus.Conn, "rpc.test.trace.Echo", "trace", nil,
		func(_ context.Context, req *natsReq) (*natsRes, error) { return &natsRes{Double: req.N}, nil }); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if _, err := rpc.NATSRequest[natsReq, natsRes](ctx, bus.Connect(t, "c"), "rpc.test.trace.Echo", &natsReq{N: 1}); err != nil {
		t.Fatal(err)
	}
	var client, server sdktrace.ReadOnlySpan
	for deadline := time.Now().Add(3 * time.Second); time.Now().Before(deadline) && (client == nil || server == nil); {
		for _, s := range rec.Ended() {
			switch s.SpanKind() {
			case trace.SpanKindClient:
				client = s
			case trace.SpanKindServer:
				server = s
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	if client == nil || server == nil {
		t.Fatal("spans not recorded")
	}
	if server.Parent().SpanID() != client.SpanContext().SpanID() || server.SpanContext().TraceID() != client.SpanContext().TraceID() {
		t.Fatal("traceparent did not cross the bus")
	}
}
