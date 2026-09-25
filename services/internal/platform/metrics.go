package platform

import (
	"net/http"
	"strconv"

	"github.com/felixge/httpsnoop"
	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// NewRegistry returns a Prometheus registry with Go runtime and process collectors. Services
// register their own collectors on it; nothing uses the global default registry, so tests can
// build many independent stacks in one process.
func NewRegistry() *prometheus.Registry {
	reg := prometheus.NewRegistry()
	reg.MustRegister(collectors.NewGoCollector(), collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}))
	return reg
}

// MetricsHandler serves /metrics for reg.
func MetricsHandler(reg *prometheus.Registry) http.Handler {
	return promhttp.HandlerFor(reg, promhttp.HandlerOpts{Registry: reg})
}

// HTTPMetrics records RED metrics (rate, errors, duration) per route pattern.
type HTTPMetrics struct {
	requests *prometheus.CounterVec
	duration *prometheus.HistogramVec
}

// NewHTTPMetrics registers the HTTP collectors for one listener.
func NewHTTPMetrics(reg prometheus.Registerer, listener string) *HTTPMetrics {
	m := &HTTPMetrics{
		requests: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name:        "helios_http_requests_total",
			Help:        "HTTP requests by route pattern and status code.",
			ConstLabels: prometheus.Labels{"listener": listener},
		}, []string{"route", "code"}),
		duration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:        "helios_http_request_duration_seconds",
			Help:        "HTTP request latency by route pattern.",
			ConstLabels: prometheus.Labels{"listener": listener},
			Buckets:     []float64{.001, .005, .01, .025, .05, .1, .25, .5, 1, 2.5, 5},
		}, []string{"route"}),
	}
	reg.MustRegister(m.requests, m.duration)
	return m
}

// Middleware must run inside a chi router so the matched route pattern is known; unmatched
// paths share one label value to bound cardinality.
func (m *HTTPMetrics) Middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		snoop := httpsnoop.CaptureMetrics(next, w, r)
		route := "unmatched"
		if rc := chi.RouteContext(r.Context()); rc != nil && rc.RoutePattern() != "" {
			route = rc.RoutePattern()
		}
		m.requests.WithLabelValues(route, strconv.Itoa(snoop.Code)).Inc()
		m.duration.WithLabelValues(route).Observe(snoop.Duration.Seconds())
	})
}
