package studio

import (
	"math"
	"strings"

	"github.com/paralin/modlock/source2/entitylump"
)

// directDynamic is a light's directlight value when the game draws its
// direct light at runtime over the baked shadows, rather than baking it into
// the irradiance.
const directDynamic = 3

// lighting reads a world's baked lighting and the stationary lights of its
// entity lumps, or returns nil for a world built without lightmaps.
func (a *Assets) lighting(data map[string]any) *Lighting {
	// Name the lightmaps by their role.
	info, _ := data["m_worldLightingInfo"].(map[string]any)
	if info == nil || info["m_bHasLightmaps"] != true {
		return nil
	}
	out := &Lighting{UvScale: vector(info["m_vLightmapUvScale"])}
	for _, item := range list(info["m_lightMaps"]) {
		name, _ := item.(string)
		switch {
		case strings.HasSuffix(name, "/directional_irradiance_sh2_dc.vtex"):
			out.Irradiance = name + "_c"
		case strings.HasSuffix(name, "/directional_irradiance_sh2_lum.vtex"):
			out.Direction = name + "_c"
		case strings.HasSuffix(name, "/direct_light_shadows.vtex"):
			out.Shadows = name + "_c"
		}
	}
	if out.Irradiance == "" {
		return nil
	}

	// Gather the stationary lights from every entity lump; a lump that
	// cannot be read adds none.
	read := func(name string) ([]byte, error) { return a.archiveOf(name).ReadFile(name) }
	for _, lump := range entitylump.Lumps(data) {
		_ = entitylump.Walk(read, lump, func(values map[string]any) {
			if light := stationary(values); light != nil {
				out.Lights = append(out.Lights, light)
			}
		})
	}
	return out
}

// stationary returns the light an entity's values describe, or nil for an
// entity that is not an enabled stationary light.
func stationary(values map[string]any) *Light {
	// Pick the light's kind by its class.
	light := &Light{}
	switch values["classname"] {
	case "light_environment":
		light.Kind = Light_KIND_SUN
	case "light_omni2":
		light.Kind = Light_KIND_POINT
	case "light_barn", "light_rect":
		light.Kind = Light_KIND_SPOT
	default:
		return nil
	}
	if values["enabled"] == false || entitylump.Number(values["directlight"]) != directDynamic {
		return nil
	}

	// The color is sRGB bytes, scaled by the brightness.
	color := list(values["color"])
	brightness := entitylump.Number(values["brightness"]) * entityNumberOr(values["brightnessscale"], 1)
	for i := range min(3, len(color)) {
		light.Color = append(light.Color, float32(linear(entitylump.Number(color[i])/255)*brightness))
	}

	// Place it and aim it along its forward axis.
	light.Origin = vector(values["origin"])
	light.Direction = forward(vector(values["angles"]))
	light.Range = float32(entitylump.Number(values["range"]))
	light.Shadow = uint32(entitylump.Number(values["bakedshadowindex"]))
	light.Inner = float32(entityNumberOr(values["inner_angle"], 180))
	light.Outer = float32(entityNumberOr(values["outer_angle"], 180))
	return light
}

// forward returns the unit forward axis of Source pitch, yaw and roll angles
// in degrees.
func forward(angles []float32) []float32 {
	if len(angles) < 2 {
		return []float32{1, 0, 0}
	}
	pitch := float64(angles[0]) * math.Pi / 180
	yaw := float64(angles[1]) * math.Pi / 180
	return []float32{
		float32(math.Cos(pitch) * math.Cos(yaw)),
		float32(math.Cos(pitch) * math.Sin(yaw)),
		float32(-math.Sin(pitch)),
	}
}

// linear converts an sRGB component to linear light.
func linear(c float64) float64 {
	if c <= 0.04045 {
		return c / 12.92
	}
	return math.Pow((c+0.055)/1.055, 2.4)
}

// vector returns a decoded list of numbers as floats.
func vector(value any) []float32 {
	items := list(value)
	out := make([]float32, len(items))
	for i, item := range items {
		out[i] = float32(entitylump.Number(item))
	}
	return out
}

// entityNumberOr returns an entity value as a number, or fallback when it
// is missing.
func entityNumberOr(value any, fallback float64) float64 {
	if value == nil {
		return fallback
	}
	return entitylump.Number(value)
}
