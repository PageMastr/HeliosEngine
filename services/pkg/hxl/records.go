package hxl

import (
	"fmt"
	"strings"

	"github.com/PageMastr/scifi-test/services/pkg/hxl/gamedef"
)

// Helpers that apply HXL and the kernel rules to the gameplay records generated from
// schemas/gameplay/*.hschema (package gamedef), so Go services validate the same content the C++
// cook accepts (engine/gameplay: AttributeLayout, ReasonCodeRegistry).

// checkAttributeFormula applies the rules of C++ AttributeLayout::bindFormula that do not need the
// layout: exactly one parameter (the entity), no context fields, no curves, and (for derived
// attributes) no stacks()/level(). Attribute and tag names are checked by the C++ host against its
// layout and tag registry.
func checkAttributeFormula(p *Program, allowContext bool) error {
	if len(p.Params()) != 1 {
		return fmt.Errorf("an attribute formula takes exactly one parameter (self), not %d", len(p.Params()))
	}
	if len(p.FieldSymbols()) != 0 {
		return fmt.Errorf("context field '%s' is not available to attribute formulas", p.FieldSymbols()[0])
	}
	if len(p.CurveSymbols()) != 0 {
		return fmt.Errorf("curve '%s' is not available to live attribute formulas", p.CurveSymbols()[0])
	}
	if !allowContext && (p.UsesStacks() || p.UsesLevel()) {
		return fmt.Errorf("stacks() and level() are not available to derived attributes")
	}
	return nil
}

// CompileDerived compiles an attribute's derived formula (params ["self"], number result) with the
// C++ rules (AttributeLayout::build): one parameter, only attributes and tags of self. It returns
// (nil, nil) when the attribute is not derived.
func CompileDerived(def *gamedef.AttributeDef) (*Program, error) {
	if def.Derived == nil {
		return nil, nil
	}
	num := Number
	p, err := Compile(string(*def.Derived), CompileOptions{Params: []string{"self"}, ExpectedType: &num})
	if err != nil {
		return nil, fmt.Errorf("attribute %s: derived formula: %w", def.Id, err)
	}
	if err := checkAttributeFormula(p, false); err != nil {
		return nil, fmt.Errorf("attribute %s: derived formula: %w", def.Id, err)
	}
	return p, nil
}

// CompileMagnitude compiles an HXL modifier magnitude (params ["self"], number result) with the C++
// rules (instantiateModifier / AttributeLayout::compileFormula): one parameter, no context fields or
// curves; stacks() and level() are allowed. (nil, nil) for the other magnitude kinds.
func CompileMagnitude(def *gamedef.ModifierDef) (*Program, error) {
	if def.Magnitude.Hxl == nil {
		return nil, nil
	}
	num := Number
	p, err := Compile(string(def.Magnitude.Hxl.Expr), CompileOptions{Params: []string{"self"}, ExpectedType: &num})
	if err != nil {
		return nil, err
	}
	if err := checkAttributeFormula(p, true); err != nil {
		return nil, fmt.Errorf("modifier magnitude: %w", err)
	}
	return p, nil
}

// ValidateReasonCode applies the reason-code rules of 06 §4 / 05 §1.6 (the same as C++
// helios::gameplay::ReasonCodeRegistry): a dotted code whose first segment names its class, caps
// only on faucets, non-negative limits.
func ValidateReasonCode(def *gamedef.ReasonCodeDef) error {
	if !isDottedName(def.Code) {
		return fmt.Errorf("invalid reason code %q", def.Code)
	}
	class := map[gamedef.ReasonKind]string{
		gamedef.ReasonKindFaucet: "Faucet", gamedef.ReasonKindSink: "Sink", gamedef.ReasonKindTransfer: "Transfer",
	}[def.Kind]
	first, _, found := strings.Cut(def.Code, ".")
	if !found || first != class {
		return fmt.Errorf("reason code %q must start with %q (its class)", def.Code, class+".")
	}
	if def.Faucet != nil && def.Kind != gamedef.ReasonKindFaucet {
		return fmt.Errorf("reason code %q: only faucets have a daily cap", def.Code)
	}
	if (def.Faucet != nil && def.Faucet.DailyCap < 0) || def.MaxTxPerHour < 0 || def.MaxAmountPerTx < 0 {
		return fmt.Errorf("reason code %q: caps and limits must be >= 0", def.Code)
	}
	return nil
}
