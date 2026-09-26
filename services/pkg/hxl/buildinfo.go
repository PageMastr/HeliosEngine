package hxl

import (
	"fmt"
	"runtime"
	"runtime/debug"
)

// GOAMD64 returns the GOAMD64 level the running binary was built with ("v1".."v4"), or "" when the
// build info is unavailable or the binary is not amd64.
func GOAMD64() string {
	if runtime.GOARCH != "amd64" {
		return ""
	}
	info, ok := debug.ReadBuildInfo()
	if !ok {
		return ""
	}
	for _, s := range info.Settings {
		if s.Key == "GOAMD64" {
			return s.Value
		}
	}
	return "v1" // the default when unset
}

// CheckGOAMD64 implements 06 §1.2 rule 6: services log the GOAMD64 level at start and refuse to run
// in production unless it is v1. Rules 1–5 already make HXL independent of FMA fusion; this is
// defence in depth. It returns nil when not in production or when the level is v1 (or not amd64).
func CheckGOAMD64(production bool) error {
	level := GOAMD64()
	if !production || level == "" || level == "v1" {
		return nil
	}
	return fmt.Errorf("hxl: this binary was built with GOAMD64=%s; production services must be built with GOAMD64=v1 (06 §1.2)", level)
}
