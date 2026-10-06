// Command helios-conformance is the plan-conformance lint (docs/plan/09-roadmap-and-process.md
// §5.10.3; tools/conformance/README.md):
//
//	go run ./cmd/helios-conformance [-root DIR] [-rules CONF-01,…] [-strict] [-sarif FILE] [-fingerprints] [-list]
//
// It exits 1 when a finding fails the run (anything not suppressed in source or covered by a
// known-failing record, plus stale records and malformed or unused suppressions), and 2 on a usage
// or I/O error.
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/PageMastr/scifi-test/tools/conformance/internal/conformance"
)

func main() {
	root := flag.String("root", ".", "repository (or fixture) root to lint")
	rules := flag.String("rules", "", "comma-separated rule IDs to run (default: all)")
	strict := flag.Bool("strict", false, "ignore tools/conformance/known_failing.jsonc")
	sarif := flag.String("sarif", "", "also write SARIF 2.1.0 to this file")
	list := flag.Bool("list", false, "list the rules and exit")
	fingerprints := flag.Bool("fingerprints", false, "end each finding's line with its fingerprint (for known_failing.jsonc)")
	flag.Parse()

	if *list {
		for _, r := range conformance.All() {
			fmt.Printf("%s  %s\n         %s\n         scope: %s\n", r.ID, r.Anchor, r.Title, strings.Join(r.Scope, ", "))
		}
		return
	}
	var ids []string
	for _, id := range strings.Split(*rules, ",") {
		if id = strings.TrimSpace(id); id != "" {
			ids = append(ids, id)
		}
	}
	res, err := conformance.Run(conformance.Options{Root: *root, Rules: ids, Strict: *strict})
	if err != nil {
		fmt.Fprintln(os.Stderr, "helios-conformance:", err)
		os.Exit(2)
	}
	res.ShowFingerprints = *fingerprints
	res.WriteText(os.Stdout)
	if *sarif != "" {
		f, err := os.Create(*sarif)
		if err == nil {
			err = res.WriteSARIF(f)
			if cerr := f.Close(); err == nil {
				err = cerr
			}
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, "helios-conformance:", err)
			os.Exit(2)
		}
	}
	if res.Failed() {
		os.Exit(1)
	}
}
