# Single image for every Helios Go service (05 §5); the entrypoint selects services with
# --services / HELIOS_SERVICES. Built from the services/ module root:
#   docker build -f deploy/Dockerfile -t helios-backend .
FROM golang:1.27.1 AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o /out/helios-backend ./cmd/helios-backend && mkdir -p /out/data

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/helios-backend /usr/local/bin/helios-backend
# Owned by nonroot so a fresh named volume mounted at /data is writable.
COPY --from=build --chown=65532:65532 /out/data /data
USER nonroot
EXPOSE 7700 7701
ENTRYPOINT ["/usr/local/bin/helios-backend", "run"]
