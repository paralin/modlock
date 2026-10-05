package mod

import "strconv"

// SettingOn returns the player's value of a switch setting the manifest
// declares.
func (p Player) SettingOn(key string) (bool, error) {
	value, err := p.Setting(key)
	return value == "true", err
}

// SettingNumber returns the player's value of a number setting the manifest
// declares.
func (p Player) SettingNumber(key string) (float64, error) {
	value, err := p.Setting(key)
	if err != nil {
		return 0, err
	}
	return strconv.ParseFloat(value, 64)
}
