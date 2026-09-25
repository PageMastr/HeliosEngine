package rpc

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"strconv"
	"time"

	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/trace"
)

// NATS header names (05 §2.1).
const (
	HeaderIdem         = "Helios-Idem"
	HeaderDeadlineMs   = "Helios-Deadline-Ms" // remaining time budget in milliseconds (relative)
	HeaderErrorCode    = "Helios-Error"
	HeaderErrorMessage = "Helios-Error-Message"
)

const tracerName = "helios/rpc"

// DefaultNATSTimeout bounds a request that carries no context deadline.
const DefaultNATSTimeout = 5 * time.Second

// NATSRequest sends req as JSON to subject and decodes the JSON reply into a new Res. The
// context deadline travels as Helios-Deadline-Ms and the trace context as traceparent.
// A reply carrying Helios-Error becomes an *Error; "no responders" becomes unavailable.
func NATSRequest[Req, Res any](ctx context.Context, nc *nats.Conn, subject string, req *Req) (*Res, error) {
	body, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}
	if _, ok := ctx.Deadline(); !ok {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, DefaultNATSTimeout)
		defer cancel()
	}
	ctx, span := otel.Tracer(tracerName).Start(ctx, subject, trace.WithSpanKind(trace.SpanKindClient))
	defer span.End()
	deadline, _ := ctx.Deadline()
	msg := nats.NewMsg(subject)
	msg.Data = body
	msg.Header.Set(HeaderDeadlineMs, strconv.FormatInt(time.Until(deadline).Milliseconds(), 10))
	otel.GetTextMapPropagator().Inject(ctx, propagation.HeaderCarrier(http.Header(msg.Header)))

	reply, err := nc.RequestMsgWithContext(ctx, msg)
	if err != nil {
		span.SetStatus(codes.Error, err.Error())
		switch {
		case errors.Is(err, nats.ErrNoResponders):
			return nil, &Error{Code: CodeUnavailable, Message: "no responders for " + subject, Cause: err}
		case errors.Is(err, context.DeadlineExceeded), errors.Is(err, nats.ErrTimeout):
			return nil, &Error{Code: CodeDeadlineExceeded, Message: "request to " + subject + " timed out", Cause: err}
		case errors.Is(err, context.Canceled):
			return nil, &Error{Code: CodeCanceled, Message: "request canceled", Cause: err}
		}
		return nil, &Error{Code: CodeUnavailable, Message: "bus request failed", Cause: err}
	}
	if code := reply.Header.Get(HeaderErrorCode); code != "" {
		span.SetStatus(codes.Error, code)
		return nil, &Error{Code: Code(code), Message: reply.Header.Get(HeaderErrorMessage)}
	}
	res := new(Res)
	if err := json.Unmarshal(reply.Data, res); err != nil {
		return nil, &Error{Code: CodeInternal, Message: "malformed reply from " + subject, Cause: err}
	}
	return res, nil
}

// NATSHandlerFunc handles one decoded NATS request.
type NATSHandlerFunc[Req, Res any] func(ctx context.Context, req *Req) (*Res, error)

// NATSHandle subscribes h to subject in queue group queue (the service name, so replicas share
// the load). Each message runs with a context bounded by the caller's Helios-Deadline-Ms.
// Messages on one subscription are handled sequentially.
func NATSHandle[Req, Res any](nc *nats.Conn, subject, queue string, log *slog.Logger, h NATSHandlerFunc[Req, Res]) (*nats.Subscription, error) {
	if log == nil {
		log = slog.Default()
	}
	return nc.QueueSubscribe(subject, queue, func(m *nats.Msg) {
		budget := DefaultNATSTimeout
		if m.Header != nil {
			if ms, err := strconv.ParseInt(m.Header.Get(HeaderDeadlineMs), 10, 64); err == nil && ms > 0 {
				budget = time.Duration(ms) * time.Millisecond
			}
		}
		ctx := context.Background()
		if m.Header != nil {
			ctx = otel.GetTextMapPropagator().Extract(ctx, propagation.HeaderCarrier(http.Header(m.Header)))
		}
		ctx, cancel := context.WithTimeout(ctx, budget)
		defer cancel()
		ctx, span := otel.Tracer(tracerName).Start(ctx, m.Subject, trace.WithSpanKind(trace.SpanKindServer))
		defer span.End()

		if m.Reply == "" {
			log.Warn("rpc request without reply subject dropped", "subject", m.Subject)
			return
		}
		req := new(Req)
		var res *Res
		err := json.Unmarshal(m.Data, req)
		if err != nil {
			err = Errorf(CodeInvalidArgument, "malformed JSON request")
		} else {
			res, err = h(ctx, req)
		}
		reply := nats.NewMsg(m.Reply)
		if err != nil {
			e := AsError(err)
			span.SetStatus(codes.Error, string(e.Code))
			if e.Code == CodeInternal || e.Code == CodeUnknown {
				log.Error("rpc internal error", "subject", m.Subject, "err", err)
			}
			reply.Header.Set(HeaderErrorCode, string(e.Code))
			reply.Header.Set(HeaderErrorMessage, e.Message)
		} else if reply.Data, err = json.Marshal(res); err != nil {
			reply.Data = nil
			reply.Header.Set(HeaderErrorCode, string(CodeInternal))
			reply.Header.Set(HeaderErrorMessage, "internal error")
		}
		if err := m.RespondMsg(reply); err != nil {
			log.Warn("rpc reply failed", "subject", m.Subject, "err", err)
		}
	})
}
