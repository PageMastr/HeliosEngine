package conformance

import (
	"encoding/json"
	"io"
)

// WriteSARIF writes the result as SARIF 2.1.0 (§5.10.3 "It writes SARIF"). Every finding is an
// `error`; a suppressed one carries an inSource suppression with its reason, and a known-failing one
// an external suppression naming its owner WP, so code scanning shows each for what it is.
func (r *Result) WriteSARIF(w io.Writer) error {
	type text struct {
		Text string `json:"text"`
	}
	type rule struct {
		ID               string            `json:"id"`
		ShortDescription text              `json:"shortDescription"`
		Properties       map[string]string `json:"properties,omitempty"`
	}
	type region struct {
		StartLine int `json:"startLine"`
	}
	type physical struct {
		ArtifactLocation map[string]string `json:"artifactLocation"`
		Region           *region           `json:"region,omitempty"`
	}
	type location struct {
		PhysicalLocation physical `json:"physicalLocation"`
	}
	type suppression struct {
		Kind          string `json:"kind"`
		Justification string `json:"justification"`
	}
	type result struct {
		RuleID       string        `json:"ruleId"`
		Level        string        `json:"level"`
		Message      text          `json:"message"`
		Locations    []location    `json:"locations"`
		Suppressions []suppression `json:"suppressions,omitempty"`
	}
	rules := []rule{{ID: ToolRule, ShortDescription: text{"The lint's own configuration, suppressions and inputs"}}}
	for _, ru := range r.Rules {
		rules = append(rules, rule{ID: ru.ID, ShortDescription: text{ru.Title},
			Properties: map[string]string{"anchor": ru.Anchor}})
	}
	results := []result{}
	for _, f := range r.Findings {
		loc := physical{ArtifactLocation: map[string]string{"uri": f.Path, "uriBaseId": "%SRCROOT%"}}
		if f.Line > 0 {
			loc.Region = &region{StartLine: f.Line}
		}
		res := result{RuleID: f.Rule, Level: "error", Message: text{f.Message}, Locations: []location{{loc}}}
		if f.Suppressed != "" {
			res.Suppressions = []suppression{{"inSource", f.Suppressed}}
		} else if f.Known != nil {
			res.Suppressions = []suppression{{"external",
				"known failing, owned by " + f.Known.Owner + " (" + f.Known.Anchor + "): " + f.Known.Reason}}
		}
		results = append(results, res)
	}
	doc := map[string]any{
		"$schema": "https://json.schemastore.org/sarif-2.1.0.json",
		"version": "2.1.0",
		"runs": []any{map[string]any{
			"tool": map[string]any{"driver": map[string]any{
				"name":           "helios-conformance",
				"informationUri": "https://github.com/PageMastr/HeliosEngine/blob/main/tools/conformance/README.md",
				"rules":          rules}},
			"results": results,
		}},
	}
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(doc)
}
