package platform

import (
	"context"
	"fmt"
	"io"
	"net/http"

	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

// SpanExporter is the hook for trace export. Phase 0 ships "none" (spans are created and
// traceparent propagates through HTTP and NATS, nothing is exported) and a line-oriented
// writer for debugging; the OTLP exporter plugs in here when compose gains the collector.
type SpanExporter = sdktrace.SpanExporter

// Telemetry owns the process-wide OpenTelemetry providers.
type Telemetry struct {
	provider *sdktrace.TracerProvider
}

// SetupTelemetry installs the global tracer provider and W3C trace-context propagation.
// debugOut receives spans when cfg.Traces is "stdout".
func SetupTelemetry(cfg TelemetryConfig, serviceName, version string, debugOut io.Writer) (*Telemetry, error) {
	res := resource.NewSchemaless(
		attribute.String("service.name", serviceName),
		attribute.String("service.version", version),
	)
	opts := []sdktrace.TracerProviderOption{
		sdktrace.WithResource(res),
		sdktrace.WithSampler(sdktrace.ParentBased(sdktrace.TraceIDRatioBased(cfg.SampleRatio))),
	}
	switch cfg.Traces {
	case "", "none":
	case "stdout":
		opts = append(opts, sdktrace.WithSyncer(&lineExporter{w: debugOut}))
	default:
		return nil, fmt.Errorf("telemetry: unknown trace exporter %q", cfg.Traces)
	}
	tp := sdktrace.NewTracerProvider(opts...)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(propagation.TraceContext{}, propagation.Baggage{}))
	return &Telemetry{provider: tp}, nil
}

// Shutdown flushes and stops the providers.
func (t *Telemetry) Shutdown(ctx context.Context) error {
	if t == nil || t.provider == nil {
		return nil
	}
	return t.provider.Shutdown(ctx)
}

// TraceHTTP wraps a handler so every request gets a server span (and extracts traceparent).
func TraceHTTP(h http.Handler, operation string) http.Handler {
	return otelhttp.NewHandler(h, operation)
}

// lineExporter writes one line per finished span; a debugging aid, not a production exporter.
type lineExporter struct{ w io.Writer }

// ExportSpans implements sdktrace.SpanExporter.
func (e *lineExporter) ExportSpans(_ context.Context, spans []sdktrace.ReadOnlySpan) error {
	for _, s := range spans {
		_, err := fmt.Fprintf(e.w, "span name=%q trace=%s span=%s parent=%s dur=%s status=%s\n",
			s.Name(), s.SpanContext().TraceID(), s.SpanContext().SpanID(), s.Parent().SpanID(),
			s.EndTime().Sub(s.StartTime()), s.Status().Code)
		if err != nil {
			return err
		}
	}
	return nil
}

// Shutdown implements sdktrace.SpanExporter.
func (e *lineExporter) Shutdown(context.Context) error { return nil }
