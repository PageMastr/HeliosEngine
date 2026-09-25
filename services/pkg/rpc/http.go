package rpc

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"math"
	"mime"
	"net/http"
	"strconv"
)

// DefaultMaxBodyBytes caps request bodies of unary JSON calls. Auth payloads are tiny; a large
// body is either a bug or an attack (05 §6.5 caps messages at 1 MiB; the edge is stricter).
const DefaultMaxBodyBytes = 64 << 10

// wireError is the Connect protocol's JSON error body.
type wireError struct {
	Code    Code   `json:"code"`
	Message string `json:"message,omitempty"`
}

// UnaryFunc is a handler for one RPC method. r gives access to headers and the client address;
// the request body has already been decoded into req.
type UnaryFunc[Req, Res any] func(ctx context.Context, r *http.Request, req *Req) (*Res, error)

// UnaryOptions tune a unary handler.
type UnaryOptions struct {
	MaxBodyBytes int64
	Logger       *slog.Logger
}

// Unary adapts a typed handler to a Connect-compatible unary JSON endpoint: POST only,
// Content-Type application/json, response 200 with the JSON message or a Connect error body
// with the mapped HTTP status. Unknown JSON fields are ignored for forward compatibility.
func Unary[Req, Res any](h UnaryFunc[Req, Res], opts UnaryOptions) http.HandlerFunc {
	limit := opts.MaxBodyBytes
	if limit <= 0 {
		limit = DefaultMaxBodyBytes
	}
	log := opts.Logger
	if log == nil {
		log = slog.Default()
	}
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			WriteError(w, r, log, Errorf(CodeUnimplemented, "method %s not allowed; use POST", r.Method))
			return
		}
		if ct := r.Header.Get("Content-Type"); ct != "" {
			mt, _, err := mime.ParseMediaType(ct)
			if err != nil || mt != "application/json" {
				WriteError(w, r, log, Errorf(CodeInvalidArgument, "content type must be application/json"))
				return
			}
		}
		req := new(Req)
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, limit))
		if err != nil {
			var tooBig *http.MaxBytesError
			if errors.As(err, &tooBig) {
				WriteError(w, r, log, Errorf(CodeResourceExhausted, "request body exceeds %d bytes", limit))
				return
			}
			WriteError(w, r, log, Errorf(CodeInvalidArgument, "cannot read request body"))
			return
		}
		if len(body) > 0 {
			if err := json.Unmarshal(body, req); err != nil {
				WriteError(w, r, log, Errorf(CodeInvalidArgument, "malformed JSON request"))
				return
			}
		}
		res, err := h(r.Context(), r, req)
		if err != nil {
			WriteError(w, r, log, err)
			return
		}
		writeJSON(w, http.StatusOK, res)
	}
}

// WriteError writes err as a Connect error body. Internal causes are logged, never sent.
func WriteError(w http.ResponseWriter, r *http.Request, log *slog.Logger, err error) {
	e := AsError(err)
	if e.Code == CodeInternal || e.Code == CodeUnknown {
		if log == nil {
			log = slog.Default()
		}
		log.ErrorContext(r.Context(), "rpc internal error", "path", r.URL.Path, "err", err)
	}
	if e.RetryAfter > 0 {
		w.Header().Set("Retry-After", strconv.Itoa(int(math.Ceil(e.RetryAfter.Seconds()))))
	}
	writeJSON(w, HTTPStatus(e.Code), wireError{Code: e.Code, Message: e.Message})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		status = http.StatusInternalServerError
		b = []byte(`{"code":"internal","message":"internal error"}`)
	}
	h := w.Header()
	h.Set("Content-Type", "application/json")
	h.Set("Cache-Control", "no-store")
	h.Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(status)
	_, _ = w.Write(b)
}

// DecodeError parses a Connect error body from an HTTP response (used by tests and Go clients).
func DecodeError(status int, body []byte) *Error {
	var we wireError
	if json.Unmarshal(body, &we) != nil || we.Code == "" {
		return &Error{Code: CodeUnknown, Message: http.StatusText(status)}
	}
	return &Error{Code: we.Code, Message: we.Message}
}
