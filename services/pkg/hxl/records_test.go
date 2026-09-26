package hxl

import (
	"encoding/json"
	"testing"

	"github.com/PageMastr/scifi-test/services/pkg/hxl/gamedef"
)

func TestRecordsCompileWithGeneratedTypes(t *testing.T) {
	var def gamedef.AttributeDef
	if err := json.Unmarshal([]byte(`{"id": "Hull.Effective", "derived": "attr(self, Hull.Hp) / (1 - attr(self, Hull.Resist))"}`), &def); err != nil {
		t.Fatal(err)
	}
	p, err := CompileDerived(&def)
	if err != nil {
		t.Fatal(err)
	}
	env := &MapEnv{Attrs: map[string]map[string]float64{"self": {"Hull.Hp": 4200, "Hull.Resist": 0.25}}}
	env.Bind(p)
	v, s := p.Eval(env)
	if s != OK || v.Number != 5600 {
		t.Fatalf("got %v %s", v.Number, s)
	}
	bad := gamedef.NewAttributeDef()
	bad.Id = "X"
	expr := gamedef.HxlExpr("ctx.speed * 2")
	bad.Derived = &expr
	if _, err := CompileDerived(&bad); err == nil {
		t.Error("derived formulas must not read context fields")
	}
	plain := gamedef.NewAttributeDef()
	if p, err := CompileDerived(&plain); p != nil || err != nil {
		t.Error("non-derived attribute")
	}
	mod := gamedef.NewModifierDef()
	mod.Magnitude = gamedef.Magnitude{Hxl: &gamedef.MagnitudeHxl{Expr: "stacks() * 5"}}
	if p, err := CompileMagnitude(&mod); err != nil || !p.UsesStacks() {
		t.Fatalf("magnitude: %v", err)
	}
}

func TestValidateReasonCode(t *testing.T) {
	ok := []gamedef.ReasonCodeDef{
		{Code: "Faucet.Bounty.NPC", Kind: gamedef.ReasonKindFaucet, Faucet: &gamedef.FaucetCap{DailyCap: 10}},
		{Code: "Sink.Tax.Market.Broker", Kind: gamedef.ReasonKindSink},
		{Code: "Transfer.Trade", Kind: gamedef.ReasonKindTransfer},
	}
	for i := range ok {
		if err := ValidateReasonCode(&ok[i]); err != nil {
			t.Errorf("%s: %v", ok[i].Code, err)
		}
	}
	bad := []gamedef.ReasonCodeDef{
		{Code: "Sink.Bounty", Kind: gamedef.ReasonKindFaucet},
		{Code: "Faucet", Kind: gamedef.ReasonKindFaucet},
		{Code: "Faucet..X", Kind: gamedef.ReasonKindFaucet},
		{Code: "Sink.Fee", Kind: gamedef.ReasonKindSink, Faucet: &gamedef.FaucetCap{DailyCap: 1}},
		{Code: "Faucet.X", Kind: gamedef.ReasonKindFaucet, MaxAmountPerTx: -1},
	}
	for i := range bad {
		if ValidateReasonCode(&bad[i]) == nil {
			t.Errorf("%s accepted", bad[i].Code)
		}
	}
}
