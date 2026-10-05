// Command helios-patch publishes builds to a CDN directory and verifies a channel the way a launcher does
// (05 §7, 08 §2.5; WP-0.16).
//
//	helios-patch publish --build DIR --product ID --channel CH --platform PL [flags]
//	    chunks DIR (FastCDC), writes new chunk objects, the signed .hman and a signed pointer with the next
//	    sequence to the CDN directory (default helios-data/cdn, 05 §5). With --channel dev and no --keys
//	    it creates throwaway dev keys in helios-data/keys/patch/<product>/ on first use.
//	helios-patch verify --product ID --channel CH --platform PL [--cdn DIR|URL] [--roots FILE] [--state FILE]
//	    runs the client-side chain: keyset against the root pair, pointer, manifest, every chunk.
//	helios-patch version
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"runtime/debug"
	"strings"
	"time"

	"github.com/PageMastr/scifi-test/services/pkg/patchcdn"
	"github.com/PageMastr/scifi-test/services/pkg/patchtrust"
)

// version is set with -ldflags "-X main.version=..." by release builds.
var version = "dev"

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Stdout, os.Stderr, time.Now))
}

type listFlag []string

func (l *listFlag) String() string     { return strings.Join(*l, ",") }
func (l *listFlag) Set(v string) error { *l = append(*l, v); return nil }

func usage(w io.Writer) {
	fmt.Fprint(w, `usage:
  helios-patch publish --build DIR --product ID --channel CH --platform PL [--data-dir DIR] [--cdn DIR]
                       [--keys DIR] [--build-id ID] [--compat-epoch N] [--min-launcher V] [--min-client V]
                       [--cdn-host URL]... [--rollout-pct N] [--tier0 PREFIX]... [--lifetime DUR]
  helios-patch verify  --product ID --channel CH --platform PL [--data-dir DIR] [--cdn DIR|URL]
                       [--roots FILE] [--state FILE]
  helios-patch version
`)
}

func buildVersion() string {
	v := version
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, s := range info.Settings {
			if s.Key == "vcs.revision" && len(s.Value) >= 12 {
				v += "+" + s.Value[:12]
			}
		}
	}
	return v
}

func run(ctx context.Context, args []string, stdout, stderr io.Writer, now func() time.Time) int {
	if len(args) == 0 {
		usage(stderr)
		return 2
	}
	cmd, args := args[0], args[1:]
	switch cmd {
	case "version":
		fmt.Fprintln(stdout, "helios-patch", buildVersion())
		return 0
	case "help", "-h", "--help":
		usage(stdout)
		return 0
	case "publish", "verify":
	default:
		fmt.Fprintf(stderr, "unknown command %q\n", cmd)
		usage(stderr)
		return 2
	}
	fs := flag.NewFlagSet("helios-patch "+cmd, flag.ContinueOnError)
	fs.SetOutput(stderr)
	var t patchtrust.Target
	fs.StringVar(&t.ProductID, "product", "", "product ID (08 §2.10.1)")
	fs.StringVar(&t.Channel, "channel", "", "channel: live, ptr, beta, dev, qa")
	fs.StringVar(&t.Platform, "platform", "", "platform: win64, linux64")
	dataDir := fs.String("data-dir", "helios-data", "dev data directory (05 §5)")
	cdn := fs.String("cdn", "", "CDN directory (verify: or an http(s):// base URL); default <data-dir>/cdn")
	var err error
	if cmd == "publish" {
		err = publish(ctx, fs, args, &t, dataDir, cdn, stdout, now)
	} else {
		err = verify(ctx, fs, args, &t, dataDir, cdn, stdout, now)
	}
	switch {
	case errors.Is(err, flag.ErrHelp):
		return 0
	case errors.Is(err, errUsage):
		return 2
	case err != nil:
		fmt.Fprintln(stderr, "helios-patch:", err)
		return 1
	}
	return 0
}

var errUsage = errors.New("usage")

func parse(fs *flag.FlagSet, args []string, t *patchtrust.Target) error {
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return err
		}
		return errUsage
	}
	if fs.NArg() != 0 || t.ProductID == "" || t.Channel == "" || t.Platform == "" {
		fmt.Fprintln(fs.Output(), "--product, --channel and --platform are required; no positional arguments")
		return errUsage
	}
	// The three become path segments (the dev keys' directory, the CDN's paths): check them first.
	if err := t.Validate(); err != nil {
		fmt.Fprintln(fs.Output(), err)
		return errUsage
	}
	return nil
}

func publish(ctx context.Context, fs *flag.FlagSet, args []string, t *patchtrust.Target, dataDir, cdn *string,
	stdout io.Writer, now func() time.Time) error {
	var o patchcdn.PublishOptions
	var hosts, tier0 listFlag
	fs.StringVar(&o.BuildDir, "build", "", "build directory to publish (required)")
	keysDir := fs.String("keys", "", "signing directory (keyset.json, manifest-key.json); default for the dev "+
		"channel: <data-dir>/keys/patch/<product>, created on first use")
	fs.StringVar(&o.BuildID, "build-id", "", "build ID; default: derived from the content")
	epoch := fs.Uint64("compat-epoch", 0, "content compat epoch (05 §1.14.1)")
	fs.StringVar(&o.MinLauncher, "min-launcher", "0", "minimum launcher version")
	fs.StringVar(&o.MinClient, "min-client", "0", "minimum client version")
	fs.Var(&hosts, "cdn-host", "CDN base URL for the pointer's cdn_hosts (repeatable)")
	pct := fs.Uint64("rollout-pct", 100, "rollout percentage 0..100")
	fs.Var(&tier0, "tier0", "path prefix of tier-0 files (repeatable; default bin/)")
	fs.DurationVar(&o.Lifetime, "lifetime", 7*24*time.Hour, "pointer lifetime, at most 168h")
	if err := parse(fs, args, t); err != nil {
		return err
	}
	if o.BuildDir == "" || *epoch > uint64(^uint32(0)) || *pct > 100 {
		fmt.Fprintln(fs.Output(), "--build is required; --compat-epoch must fit 32 bits; --rollout-pct is 0..100")
		return errUsage
	}
	o.Target, o.CompatEpoch, o.RolloutPct, o.CDNHosts, o.Now = *t, uint32(*epoch), uint8(*pct), hosts, now()
	if len(tier0) > 0 {
		o.Tier0 = tier0
	}
	o.CDNRoot = *cdn
	if o.CDNRoot == "" {
		o.CDNRoot = filepath.Join(*dataDir, "cdn")
	}
	var keys *patchcdn.Keys
	var err error
	switch {
	case *keysDir != "":
		keys, err = patchcdn.LoadKeys(*keysDir, t.ProductID)
	case t.Channel == "dev":
		dir := filepath.Join(*dataDir, "keys", "patch", t.ProductID)
		var created bool
		keys, created, err = patchcdn.LoadOrCreateDevKeys(dir, t.ProductID, o.Now)
		if created {
			fmt.Fprintf(stdout, "created dev keys in %s (throwaway; never used for live)\n", dir)
		}
	default:
		return fmt.Errorf("--keys is required for channel %q (dev keys sign only the dev channel)", t.Channel)
	}
	if err != nil {
		return err
	}
	res, err := patchcdn.Publish(ctx, o, keys)
	if err != nil {
		return err
	}
	fmt.Fprintf(stdout, "published %s/%s/%s build %s: %d files, %d chunks (%d written, %d bytes), manifest %s%s, "+
		"pointer sequence %d%s\n", t.ProductID, t.Channel, t.Platform, res.BuildID, res.Files, res.Chunks,
		res.ChunksWritten, res.BytesWritten, res.ManifestHash, map[bool]string{false: " (already published)"}[res.ManifestWritten],
		res.Sequence, map[bool]string{false: " (unchanged)"}[res.PointerWritten])
	return nil
}

func verify(ctx context.Context, fs *flag.FlagSet, args []string, t *patchtrust.Target, dataDir, cdn *string,
	stdout io.Writer, now func() time.Time) error {
	roots := fs.String("roots", "", "roots.json with the product's root pair; default "+
		"<data-dir>/keys/patch/<product>/roots.json (the dev keys')")
	statePath := fs.String("state", "", "ratchet state file, loaded and saved as an install would; default: "+
		"a fresh install's state, not saved")
	if err := parse(fs, args, t); err != nil {
		return err
	}
	if *roots == "" {
		*roots = filepath.Join(*dataDir, "keys", "patch", t.ProductID, "roots.json")
	}
	product, rp, err := patchcdn.LoadRoots(*roots)
	if err != nil {
		return err
	}
	if product != t.ProductID {
		return fmt.Errorf("%s holds %q's roots, not %q's", *roots, product, t.ProductID)
	}
	v, err := patchtrust.NewVerifier(*t, rp, patchtrust.Options{})
	if err != nil {
		return err
	}
	var src patchcdn.Source = patchcdn.DirSource{Root: filepath.Join(*dataDir, "cdn")}
	switch {
	case strings.HasPrefix(*cdn, "http://") || strings.HasPrefix(*cdn, "https://"):
		src = patchcdn.HTTPSource{Base: *cdn}
	case *cdn != "":
		src = patchcdn.DirSource{Root: *cdn}
	}
	var store patchtrust.StateStore = &patchcdn.MemoryStateStore{}
	if *statePath != "" {
		store = patchtrust.FileStateStore{Path: *statePath}
	}
	res, err := patchcdn.Verify(ctx, src, v, uint64(now().Unix()), store)
	if err != nil {
		return err
	}
	p := res.Pointer
	fmt.Fprintf(stdout, "verified %s/%s/%s: keyset v%d (root epoch %d), pointer sequence %d (expires %s), build %s "+
		"(manifest %s): %d files, %d chunks, %d bytes\n", t.ProductID, t.Channel, t.Platform, res.Keyset.Version,
		res.Keyset.RootEpoch, p.Sequence, time.Unix(int64(p.Expires), 0).UTC().Format(time.RFC3339), p.BuildID,
		p.ManifestHash, len(res.Manifest.Files), res.Chunks, res.RawBytes)
	return nil
}
