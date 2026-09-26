// Command hxlfloat runs the 06 §1.2 float-rule analyzer on package directories:
//
//	go run ./pkg/hxl/hxlfloat/cmd/hxlfloat ./pkg/hxl/...
//	go run ./pkg/hxl/hxlfloat/cmd/hxlfloat ./pkg/hxl ./pkg/hxl/det
//
// A trailing "/..." checks every package directory below (testdata excluded), like the go tool. It
// prints one line per finding and exits with status 1 if there are any (CI, pre-commit hook).
package main

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"github.com/PageMastr/scifi-test/services/pkg/hxl/hxlfloat"
)

// expand turns "dir/..." into every directory below dir that holds .go files.
func expand(arg string) ([]string, error) {
	root, recursive := strings.CutSuffix(arg, "/...")
	if !recursive {
		return []string{arg}, nil
	}
	var dirs []string
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() {
			return nil
		}
		if d.Name() == "testdata" {
			return filepath.SkipDir
		}
		if matches, _ := filepath.Glob(filepath.Join(path, "*.go")); len(matches) > 0 {
			dirs = append(dirs, path)
		}
		return nil
	})
	return dirs, err
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: hxlfloat <package dir | dir/...>...")
		os.Exit(2)
	}
	count := 0
	for _, arg := range os.Args[1:] {
		dirs, err := expand(arg)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		for _, dir := range dirs {
			findings, err := hxlfloat.CheckDir(dir)
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(2)
			}
			for _, f := range findings {
				fmt.Println(f)
				count++
			}
		}
	}
	if count > 0 {
		os.Exit(1)
	}
}
