package rpc_test

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/nats-io/nats-server/v2/server"
	"github.com/nats-io/nats.go"
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

// seamConn is a handler's connection to the test server with two faults a test can inject:
// writes carrying a SUB for an rpc.test.live.* subject are held back by subDelay (a loaded host
// on which the client's flusher or the server's read loop runs late), and once dropPings is set
// the client's PINGs are swallowed, so no flush can complete.
type seamConn struct {
	net.Conn
	subDelay  time.Duration
	dropPings atomic.Bool
	dropped   chan struct{} // signalled when a PING was swallowed
}

func (c *seamConn) Write(p []byte) (int, error) {
	if bytes.Contains(p, []byte("SUB rpc.test.live.")) {
		time.Sleep(c.subDelay)
	}
	if !c.dropPings.Load() || !bytes.Contains(p, []byte("PING\r\n")) {
		return c.Conn.Write(p)
	}
	if rest := bytes.ReplaceAll(p, []byte("PING\r\n"), nil); len(rest) > 0 {
		if _, err := c.Conn.Write(rest); err != nil {
			return 0, err
		}
	}
	select {
	case c.dropped <- struct{}{}:
	default:
	}
	return len(p), nil
}

type seamServer struct {
	bus  *testkit.NATS
	conn *seamConn
}

func (s *seamServer) InProcessConn() (net.Conn, error) {
	c, err := s.bus.Server.InProcessConn()
	if err != nil {
		return nil, err
	}
	s.conn.Conn = c
	return s.conn, nil
}

// connectSeam opens a non-reconnecting in-process connection through a seamConn.
func connectSeam(t *testing.T, bus *testkit.NATS, subDelay time.Duration) (*nats.Conn, *seamConn) {
	t.Helper()
	sc := &seamConn{subDelay: subDelay, dropped: make(chan struct{}, 1)}
	nc, err := nats.Connect("", nats.InProcessServer(&seamServer{bus: bus, conn: sc}), nats.NoReconnect(),
		nats.Name("seam"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(nc.Close)
	return nc, sc
}

// Regression test for the TestNATSRoundTrip flake (CI run 36297552364, about 1 in 1000 runs
// locally): NATSHandle returned while its SUB still sat in the client's write buffer, so a
// request from another connection could reach the server first and get no responders. Holding
// the SUB back makes that ordering certain.
func TestNATSHandleIsLiveOnReturn(t *testing.T) {
	bus := testkit.StartNATS(t)
	nc, _ := connectSeam(t, bus, 200*time.Millisecond)
	client := bus.Connect(t, "client")
	if _, err := rpc.NATSHandle(nc, "rpc.test.live.Double", "live", nil,
		func(_ context.Context, req *natsReq) (*natsRes, error) { return &natsRes{Double: 2 * req.N}, nil }); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	res, err := rpc.NATSRequest[natsReq, natsRes](ctx, client, "rpc.test.live.Double", &natsReq{N: 4})
	if err != nil || res.Double != 8 {
		t.Fatalf("request right after NATSHandle returned: %+v %v", res, err)
	}
}

// A subscription the server never confirmed must be reported as a failure, not handed back as
// if it were serving.
func TestNATSHandleUnconfirmed(t *testing.T) {
	bus := testkit.StartNATS(t)
	nc, sc := connectSeam(t, bus, 0)
	sc.dropPings.Store(true)
	go func() {
		<-sc.dropped // NATSHandle is now waiting for a PONG that cannot come
		nc.Close()
	}()
	sub, err := rpc.NATSHandle(nc, "rpc.test.live.Lost", "live", nil,
		func(context.Context, *natsReq) (*natsRes, error) { return &natsRes{}, nil })
	if sub != nil || !errors.Is(err, nats.ErrConnectionClosed) {
		t.Fatalf("unconfirmed subscription: returned a subscription: %t, err: %v", sub != nil, err)
	}
}

// The timeout branch on a live connection: the PING is swallowed but nc stays open, so the
// subscription must be removed from nc and from the server.
func TestNATSHandleTimeoutRemovesSubscription(t *testing.T) {
	const timeout = 100 * time.Millisecond
	defer rpc.SetNATSConfirmTimeout(timeout)()
	bus := testkit.StartNATS(t)
	nc, sc := connectSeam(t, bus, 0)
	client := bus.Connect(t, "client")
	marker, err := client.SubscribeSync("test.marker")
	if err != nil {
		t.Fatal(err)
	}
	if err := client.Flush(); err != nil {
		t.Fatal(err)
	}
	sc.dropPings.Store(true)
	start := time.Now()
	sub, err := rpc.NATSHandle(nc, "rpc.test.live.Slow", "live", nil, double)
	if sub != nil || !errors.Is(err, nats.ErrTimeout) {
		t.Fatalf("returned a subscription: %t, err: %v", sub != nil, err)
	}
	if took := time.Since(start); took < timeout {
		t.Fatalf("returned after %v, before the %v confirm timeout", took, timeout)
	}
	if n := nc.NumSubscriptions(); n != 0 {
		t.Fatalf("nc still holds %d subscriptions", n)
	}
	// nc cannot flush any more (the swallowed PING leaves its PONG queue one entry behind), so
	// order through the connection instead: once the server forwards a message nc published
	// after the UNSUB, it has processed the UNSUB.
	if err := nc.Publish("test.marker", nil); err != nil {
		t.Fatal(err)
	}
	if _, err := marker.NextMsg(3 * time.Second); err != nil {
		t.Fatalf("marker: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, err = rpc.NATSRequest[natsReq, natsRes](ctx, client, "rpc.test.live.Slow", &natsReq{N: 1})
	if rpc.CodeOf(err) != rpc.CodeUnavailable {
		t.Fatalf("the server still routes to the unconfirmed handler: %v", err)
	}
}

func double(_ context.Context, req *natsReq) (*natsRes, error) {
	return &natsRes{Double: 2 * req.N}, nil
}

func permOptions(fleetDeny ...string) *server.Options {
	return &server.Options{ServerName: "perm", DontListen: true, NoLog: true, NoSigs: true,
		Users: []*server.User{
			{Username: "svc", Password: "pw"},
			{Username: "fleet", Password: "pw", Permissions: &server.Permissions{
				Subscribe: &server.SubjectPermission{Deny: fleetDeny}}},
		}}
}

// startPermNATS boots an in-process server on which "svc" may do anything and "fleet" may not
// subscribe to rpc.> nor join queue group "other" on grp.test.Double.
func startPermNATS(t *testing.T) *server.Server {
	t.Helper()
	ns, err := server.NewServer(permOptions("rpc.>", "grp.test.Double other"))
	if err != nil {
		t.Fatal(err)
	}
	go ns.Start()
	if !ns.ReadyForConnections(10 * time.Second) {
		ns.Shutdown()
		t.Fatal("permissions server not ready")
	}
	t.Cleanup(func() { ns.Shutdown(); ns.WaitForShutdown() })
	return ns
}

// connectAs connects user to ns. Refusals only go to a silent async error handler (the
// default one prints them).
func connectAs(t *testing.T, ns *server.Server, user string) *nats.Conn {
	t.Helper()
	nc, err := nats.Connect("", nats.InProcessServer(ns), nats.UserInfo(user, "pw"), nats.Name(user),
		nats.ErrorHandler(func(*nats.Conn, *nats.Subscription, error) {}))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(nc.Close)
	return nc
}

// requestDouble asks subject to double n from nc and fails the test unless it gets 2n back.
func requestDouble(t *testing.T, nc *nats.Conn, subject string, n int) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	res, err := rpc.NATSRequest[natsReq, natsRes](ctx, nc, subject, &natsReq{N: n})
	if err != nil || res.Double != 2*n {
		t.Fatalf("request to %s: %+v %v", subject, res, err)
	}
}

// A SUB the server refuses is followed by a PONG like any other, so the round trip alone would
// hand back a dead handler as serving.
func TestNATSHandleRefusedSubscription(t *testing.T) {
	ns := startPermNATS(t)
	fleet := connectAs(t, ns, "fleet")
	for _, c := range []struct{ subject, queue string }{
		{"rpc.test.perm.Double", "perm"}, // subject denied
		{"grp.test.Double", "other"},     // only this queue group denied
	} {
		sub, err := rpc.NATSHandle(fleet, c.subject, c.queue, nil, double)
		if sub != nil || !errors.Is(err, nats.ErrPermissionViolation) {
			t.Fatalf("%s in %s: returned a subscription: %t, err: %v", c.subject, c.queue, sub != nil, err)
		}
		if n := fleet.NumSubscriptions(); n != 0 {
			t.Fatalf("%s in %s: fleet still holds %d subscriptions", c.subject, c.queue, n)
		}
	}
}

// An earlier refusal of another SUB that LastError still holds must not fail NATSHandle.
func TestNATSHandleIgnoresUnrelatedRefusal(t *testing.T) {
	ns := startPermNATS(t)
	fleet, svc := connectAs(t, ns, "fleet"), connectAs(t, ns, "svc")
	for _, c := range []struct{ subject, queue string }{
		{"rpc.test.perm.Other", "perm"}, // another subject
		{"grp.test.Double", "other"},    // the same subject in another queue group
	} {
		if _, err := fleet.QueueSubscribe(c.subject, c.queue, func(*nats.Msg) {}); err != nil {
			t.Fatal(err)
		}
		if err := fleet.Flush(); err != nil {
			t.Fatal(err)
		}
		if !errors.Is(fleet.LastError(), nats.ErrPermissionViolation) {
			t.Fatalf("setup: %s in %s was not refused: %v", c.subject, c.queue, fleet.LastError())
		}
		sub, err := rpc.NATSHandle(fleet, "grp.test.Double", "live", nil, double)
		if err != nil {
			t.Fatalf("after a refusal of %s in %s: %v", c.subject, c.queue, err)
		}
		requestDouble(t, svc, "grp.test.Double", 3)
		if err := sub.Unsubscribe(); err != nil {
			t.Fatal(err)
		}
	}
}

// A refusal of the same subject and queue from an earlier attempt, still held by LastError,
// must not fail a later SUB that the server accepts (here after a permissions reload).
func TestNATSHandleIgnoresStaleRefusal(t *testing.T) {
	ns := startPermNATS(t)
	fleet, svc := connectAs(t, ns, "fleet"), connectAs(t, ns, "svc")
	_, err := rpc.NATSHandle(fleet, "rpc.test.perm.Double", "perm", nil, double)
	if !errors.Is(err, nats.ErrPermissionViolation) {
		t.Fatalf("setup: not refused: %v", err)
	}
	if err := ns.ReloadOptions(permOptions()); err != nil {
		t.Fatal(err)
	}
	if !errors.Is(fleet.LastError(), nats.ErrPermissionViolation) {
		t.Fatalf("setup: LastError no longer holds the refusal: %v", fleet.LastError())
	}
	if _, err := rpc.NATSHandle(fleet, "rpc.test.perm.Double", "perm", nil, double); err != nil {
		t.Fatalf("accepted after the reload: %v", err)
	}
	requestDouble(t, svc, "rpc.test.perm.Double", 5)
}

// refusedSubscription must match only a permissions violation for this subject, and for this
// queue group when the text names one. The texts are the server's (nats-server client.go), as
// nats.go wraps them.
func TestRefusedSubscriptionMatch(t *testing.T) {
	violation := func(text string) error { return fmt.Errorf("%w: %s", nats.ErrPermissionViolation, text) }
	cases := []struct {
		err   error
		queue string
		want  bool
	}{
		{violation(`Permissions Violation for Subscription to "a.b" using queue "q"`), "q", true},
		{violation(`Permissions Violation for Subscription to "a.b" using queue "q" (sid "7")`), "q", true},
		{violation(`Permissions Violation for Subscription to "a.b", too many tokens`), "q", true},
		{violation(`Permissions Violation for Subscription to "a.b"`), "q", true},
		{violation(`Permissions Violation for Subscription to "a.b"`), "", true},
		{violation(`Permissions Violation for Subscription to "a.b" using queue "other"`), "q", false},
		{violation(`Permissions Violation for Subscription to "a.b" using queue "q"`), "", false},
		{violation(`Permissions Violation for Subscription to "a.b.c" using queue "q"`), "q", false},
		{violation(`Permissions Violation for Subscription to "a"`), "q", false},
		{violation(`Permissions Violation for Publish to "a.b"`), "q", false},
		{errors.New(`Permissions Violation for Subscription to "a.b" using queue "q"`), "q", false}, // not nats's
		{nats.ErrMaxSubscriptionsExceeded, "q", false},
		{nil, "q", false},
	}
	for _, c := range cases {
		if got := rpc.RefusedSubscription(c.err, "a.b", c.queue); got != c.want {
			t.Errorf("%v with queue %q: got %t", c.err, c.queue, got)
		}
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
