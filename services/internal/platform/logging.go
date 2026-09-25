package platform

import (
	"io"
	"log/slog"
	"strings"
)

// ParseLevel maps a config level name to a slog level (unknown names mean info).
func ParseLevel(s string) slog.Level {
	switch strings.ToLower(s) {
	case "debug":
		return slog.LevelDebug
	case "warn", "warning":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

// NewLogger builds the process logger: JSON lines for Loki in production (05 §6.2), text for a
// developer console. Every record carries the shard so multi-shard logs stay separable.
func NewLogger(cfg LogConfig, shard string, w io.Writer) *slog.Logger {
	opts := &slog.HandlerOptions{Level: ParseLevel(cfg.Level)}
	var h slog.Handler
	if cfg.Format == "json" {
		h = slog.NewJSONHandler(w, opts)
	} else {
		h = slog.NewTextHandler(w, opts)
	}
	return slog.New(h).With("shard", shard)
}

// Service returns a child logger tagged with a service name.
func Service(log *slog.Logger, name string) *slog.Logger { return log.With("svc", name) }
