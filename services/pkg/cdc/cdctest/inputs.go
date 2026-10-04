// Package cdctest generates the inputs of the shared FastCDC and .hman test vectors
// (services/testdata/vectors/fastcdc.json and hman/), exactly as engine/patch's doctest helper
// (engine/patch/tests/patch_test_util.h) does, so both test suites chunk the same bytes.
//
// Kinds:
//
//	random  Size bytes: the SplitMix64(Seed) outputs, each as 8 little-endian bytes, truncated to Size
//	zero    Size zero bytes
//	repeat  random(Seed, Period) repeated and truncated to Size
//	insert  random(Seed, Size) with random(InsertSeed, InsertSize) inserted before byte At
//
// Edits, if any, then overwrite bytes of the result (each the bytes of Hex, from offset At): crafted
// inputs, such as a gear-hash match at an exact offset.
//
// Functions here are safe for concurrent use.
package cdctest

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"

	"github.com/PageMastr/scifi-test/services/pkg/cdc"
)

// Input describes one generated input (the "input" object of a vector).
type Input struct {
	Kind       string `json:"kind"`
	Seed       uint64 `json:"seed,omitempty"`
	Size       int    `json:"size"`
	Period     int    `json:"period,omitempty"`
	At         int    `json:"at,omitempty"`
	InsertSize int    `json:"insertSize,omitempty"`
	InsertSeed uint64 `json:"insertSeed,omitempty"`
	Edits      []Edit `json:"edits,omitempty"`
}

// Edit overwrites len(Hex)/2 bytes at At with the bytes Hex spells.
type Edit struct {
	At  int    `json:"at"`
	Hex string `json:"hex"`
}

// Random returns size bytes of the SplitMix64(seed) stream.
func Random(seed uint64, size int) []byte {
	out := make([]byte, (size+7)/8*8)
	s := cdc.SplitMix64{State: seed}
	for i := 0; i < len(out); i += 8 {
		binary.LittleEndian.PutUint64(out[i:], s.Next())
	}
	return out[:size]
}

// Bytes generates the input.
func (in Input) Bytes() ([]byte, error) {
	out, err := in.generate()
	if err != nil {
		return nil, err
	}
	for _, e := range in.Edits {
		b, err := hex.DecodeString(e.Hex)
		if err != nil || e.At < 0 || e.At > len(out)-len(b) {
			return nil, fmt.Errorf("cdctest: edit %q at %d of %d bytes is invalid", e.Hex, e.At, len(out))
		}
		copy(out[e.At:], b)
	}
	return out, nil
}

func (in Input) generate() ([]byte, error) {
	if in.Size < 0 {
		return nil, fmt.Errorf("cdctest: negative size %d", in.Size)
	}
	switch in.Kind {
	case "random":
		return Random(in.Seed, in.Size), nil
	case "zero":
		return make([]byte, in.Size), nil
	case "repeat":
		if in.Period <= 0 {
			return nil, fmt.Errorf("cdctest: repeat needs a positive period")
		}
		unit := Random(in.Seed, in.Period)
		out := make([]byte, in.Size)
		for i := 0; i < len(out); i += len(unit) {
			copy(out[i:], unit)
		}
		return out, nil
	case "insert":
		if in.At < 0 || in.At > in.Size || in.InsertSize < 0 {
			return nil, fmt.Errorf("cdctest: insert at %d of %d bytes is out of range", in.At, in.Size)
		}
		base := Random(in.Seed, in.Size)
		out := make([]byte, 0, in.Size+in.InsertSize)
		out = append(out, base[:in.At]...)
		out = append(out, Random(in.InsertSeed, in.InsertSize)...)
		return append(out, base[in.At:]...), nil
	}
	return nil, fmt.Errorf("cdctest: unknown input kind %q", in.Kind)
}
