// Package rpc holds the transport-neutral error model and the two Phase 0 transports:
// Connect-protocol-compatible JSON unary handlers over HTTP (launcher, tools) and JSON
// request/reply over NATS (gateways and cells, 05 §2.1).
//
// Handlers are written once against Error codes; both transports map them to their wire form.
// When schemac starts emitting .proto files the HTTP side is swapped for generated connect-go
// handlers without changing paths, JSON field names or error bodies seen by clients.
package rpc

import (
	"errors"
	"fmt"
	"net/http"
	"time"
)

// Code is a Connect/gRPC status code name.
type Code string

// Error codes (the Connect protocol's canonical names).
const (
	CodeCanceled           Code = "canceled"
	CodeUnknown            Code = "unknown"
	CodeInvalidArgument    Code = "invalid_argument"
	CodeDeadlineExceeded   Code = "deadline_exceeded"
	CodeNotFound           Code = "not_found"
	CodeAlreadyExists      Code = "already_exists"
	CodePermissionDenied   Code = "permission_denied"
	CodeResourceExhausted  Code = "resource_exhausted"
	CodeFailedPrecondition Code = "failed_precondition"
	CodeAborted            Code = "aborted"
	CodeUnimplemented      Code = "unimplemented"
	CodeInternal           Code = "internal"
	CodeUnavailable        Code = "unavailable"
	CodeUnauthenticated    Code = "unauthenticated"
)

// Error is an RPC failure with a code safe to show to the caller. Message must never contain
// secrets or internals; wrap the underlying cause in Cause for logs only.
type Error struct {
	Code       Code
	Message    string
	RetryAfter time.Duration // set for resource_exhausted; becomes a Retry-After header
	Cause      error         // logged, never sent
}

// Error implements error; it includes the cause, so use it for logs only.
func (e *Error) Error() string {
	if e.Cause != nil {
		return fmt.Sprintf("%s: %s: %v", e.Code, e.Message, e.Cause)
	}
	return fmt.Sprintf("%s: %s", e.Code, e.Message)
}

// Unwrap returns the internal cause.
func (e *Error) Unwrap() error { return e.Cause }

// Errorf builds an Error with a formatted message.
func Errorf(code Code, format string, args ...any) *Error {
	return &Error{Code: code, Message: fmt.Sprintf(format, args...)}
}

// Internal wraps an unexpected failure. The caller sees only "internal error".
func Internal(cause error) *Error {
	return &Error{Code: CodeInternal, Message: "internal error", Cause: cause}
}

// AsError converts any error into an *Error (unknown errors become internal).
func AsError(err error) *Error {
	if err == nil {
		return nil
	}
	var e *Error
	if errors.As(err, &e) {
		return e
	}
	return Internal(err)
}

// CodeOf returns the code of err ("" for nil).
func CodeOf(err error) Code {
	if err == nil {
		return ""
	}
	return AsError(err).Code
}

// HTTPStatus maps a code to the HTTP status the Connect protocol uses for it.
func HTTPStatus(c Code) int {
	switch c {
	case CodeCanceled:
		return 499
	case CodeInvalidArgument, CodeFailedPrecondition:
		return http.StatusBadRequest
	case CodeDeadlineExceeded:
		return http.StatusGatewayTimeout
	case CodeNotFound:
		return http.StatusNotFound
	case CodeAlreadyExists, CodeAborted:
		return http.StatusConflict
	case CodePermissionDenied:
		return http.StatusForbidden
	case CodeResourceExhausted:
		return http.StatusTooManyRequests
	case CodeUnimplemented:
		return http.StatusNotImplemented
	case CodeUnavailable:
		return http.StatusServiceUnavailable
	case CodeUnauthenticated:
		return http.StatusUnauthorized
	default:
		return http.StatusInternalServerError
	}
}
