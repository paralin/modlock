// Package settings holds the rules for the settings a mod declares in its
// manifest: which declarations a build may carry, which values each setting
// accepts, and the value a player starts with. Hosts without a settings
// service keep players' values in a File.
package settings

import (
	"math"
	"regexp"
	"slices"
	"strconv"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
)

const (
	// limit bounds how many settings a mod declares.
	limit = 32
	// choiceLimit bounds the choices of one setting.
	choiceLimit = 16
	// labelLimit bounds a setting or choice label, in bytes.
	labelLimit = 64
)

var (
	// keyPattern matches a setting key.
	keyPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]{0,63}$`)
	// valuePattern matches a choice value.
	valuePattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,32}$`)
)

// Find returns the setting named key that manifest declares, or nil.
func Find(manifest *wasm.Manifest, key string) *wasm.Setting {
	for _, setting := range manifest.GetSettings() {
		if setting.GetKey() == key {
			return setting
		}
	}
	return nil
}

// Canonical returns value as the setting holds it and whether the setting
// accepts it: a choice's value, "true" or "false", or a number from min to
// max on a step from min.
func Canonical(setting *wasm.Setting, value string) (string, bool) {
	switch setting.GetKind() {
	case wasm.Setting_KIND_CHOICE:
		ok := slices.ContainsFunc(setting.GetChoices(), func(c *wasm.SettingChoice) bool { return c.GetValue() == value })
		return value, ok
	case wasm.Setting_KIND_SWITCH:
		return value, value == "true" || value == "false"
	case wasm.Setting_KIND_NUMBER:
		number, err := strconv.ParseFloat(value, 64)
		if err != nil || math.IsNaN(number) || number < setting.GetMin() || number > setting.GetMax() {
			return "", false
		}
		if step := setting.GetStep(); step > 0 {
			steps := (number - setting.GetMin()) / step
			if math.Abs(steps-math.Round(steps)) > 1e-9 {
				return "", false
			}
		}
		return numberText(number), true
	}
	return "", false
}

// Default returns the value a player starts with: the declared default, or
// the first choice, off or min when the setting declares none.
func Default(setting *wasm.Setting) string {
	if value, ok := Canonical(setting, setting.GetDefaultValue()); ok {
		return value
	}
	switch setting.GetKind() {
	case wasm.Setting_KIND_CHOICE:
		if len(setting.GetChoices()) != 0 {
			return setting.GetChoices()[0].GetValue()
		}
	case wasm.Setting_KIND_SWITCH:
		return "false"
	case wasm.Setting_KIND_NUMBER:
		return numberText(setting.GetMin())
	}
	return ""
}

// Validate checks the settings manifest declares.
func Validate(manifest *wasm.Manifest) error {
	settings := manifest.GetSettings()
	if len(settings) > limit {
		return errors.Errorf("mod.json declares more than %d settings", limit)
	}
	for i, setting := range settings {
		if err := validate(setting); err != nil {
			return err
		}
		if slices.ContainsFunc(settings[:i], func(s *wasm.Setting) bool { return s.GetKey() == setting.GetKey() }) {
			return errors.Errorf("mod.json declares setting %q twice", setting.GetKey())
		}
	}
	return nil
}

// validate checks one declared setting.
func validate(setting *wasm.Setting) error {
	// Check the name the code knows the setting by and the one players see.
	key := setting.GetKey()
	if !keyPattern.MatchString(key) {
		return errors.Errorf("setting %q key must be up to 64 letters, digits and underscores, not starting with a digit", key)
	}
	if label := setting.GetLabel(); label == "" || len(label) > labelLimit {
		return errors.Errorf("setting %q needs a label of at most %d bytes", key, labelLimit)
	}

	// Check the values its kind allows.
	switch setting.GetKind() {
	case wasm.Setting_KIND_CHOICE:
		choices := setting.GetChoices()
		if len(choices) == 0 || len(choices) > choiceLimit {
			return errors.Errorf("setting %q lists 1 to %d choices", key, choiceLimit)
		}
		for i, choice := range choices {
			if !valuePattern.MatchString(choice.GetValue()) || len(choice.GetLabel()) > labelLimit {
				return errors.Errorf("setting %q choice %q must be a short word with a short label", key, choice.GetValue())
			}
			if slices.ContainsFunc(choices[:i], func(c *wasm.SettingChoice) bool { return c.GetValue() == choice.GetValue() }) {
				return errors.Errorf("setting %q lists choice %q twice", key, choice.GetValue())
			}
		}
	case wasm.Setting_KIND_SWITCH:
	case wasm.Setting_KIND_NUMBER:
		if !(setting.GetMin() < setting.GetMax()) || setting.GetStep() < 0 {
			return errors.Errorf("setting %q needs a min below its max and a step of zero or more", key)
		}
	default:
		return errors.Errorf("setting %q kind must be KIND_CHOICE, KIND_SWITCH or KIND_NUMBER", key)
	}

	// Check that a declared default is one of those values.
	if value := setting.GetDefaultValue(); value != "" {
		if _, ok := Canonical(setting, value); !ok {
			return errors.Errorf("setting %q default %q is not one of its values", key, value)
		}
	}
	return nil
}

// numberText writes the shortest text that reads back as number.
func numberText(number float64) string {
	return strconv.FormatFloat(number, 'g', -1, 64)
}
