// SPDX-License-Identifier: GPL-3.0-or-later

// Draws Melee Tactics' app icon: a shine, the glowing blue hexagon of a
// reflector, on a deep night-blue field (an original drawing, not Nintendo's
// art). Full bleed, so iOS and Android can round or mask the corners
// themselves.
//
//	go run ./tools/icon platforms/browser/icons
package main

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
)

type rgba struct{ r, g, b, a float64 }

func over(dst, src rgba) rgba {
	a := src.a + dst.a*(1-src.a)
	if a == 0 {
		return rgba{}
	}
	mix := func(d, s float64) float64 { return (s*src.a + d*dst.a*(1-src.a)) / a }
	return rgba{mix(dst.r, src.r), mix(dst.g, src.g), mix(dst.b, src.b), a}
}

// Light added on top, as a glow is.
func add(dst, light rgba) rgba {
	return rgba{math.Min(1, dst.r+light.r*light.a), math.Min(1, dst.g+light.g*light.a),
		math.Min(1, dst.b+light.b*light.a), dst.a}
}

func lerp(a, b rgba, t float64) rgba {
	t = math.Max(0, math.Min(1, t))
	return rgba{a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t, a.b + (b.b-a.b)*t, a.a + (b.a-a.a)*t}
}

func hex(v uint32) rgba {
	return rgba{float64(v>>16&0xFF) / 255, float64(v>>8&0xFF) / 255, float64(v&0xFF) / 255, 1}
}

func withAlpha(c rgba, a float64) rgba {
	c.a = math.Max(0, math.Min(1, a))
	return c
}

// Signed distance from (x, y) to a hexagon of inradius r centred on the
// origin, pointy at the top and bottom: negative inside.
func hexagon(x, y, r float64) float64 {
	// The flat-topped hexagon's distance, with x and y swapped.
	px, py := math.Abs(y), math.Abs(x)
	const kx, ky, kz = -0.866025404, 0.5, 0.577350269
	d := math.Min(kx*px+ky*py, 0)
	px -= 2 * d * kx
	py -= 2 * d * ky
	cx := math.Max(-kz*r, math.Min(kz*r, px))
	px -= cx
	py -= r
	l := math.Hypot(px, py)
	if py < 0 {
		return -l
	}
	return l
}

// One sample of the icon at (x, y) in [0, 1].
func sample(x, y float64) rgba {
	cx, cy := x-0.5, y-0.5
	// The field: night blue, a little lighter behind the shine.
	c := lerp(hex(0x16245a), hex(0x03050f), math.Hypot(cx, cy)/0.7)

	const r = 0.3
	d := hexagon(cx, cy, r)
	// The glow around it, fading out from the edge.
	if d > 0 {
		c = add(c, withAlpha(hex(0x3aa8ff), 0.85*math.Exp(-d/0.055)))
	}
	if d <= 0 {
		// The body: pale at the heart, deepening to blue at the rim, with
		// a second, fainter hexagon inside it.
		depth := -d / r
		body := lerp(hex(0x2a6cff), hex(0xd8fbff), depth*1.4)
		c = over(c, withAlpha(body, 0.92))
		if inner := hexagon(cx, cy, r*0.55); math.Abs(inner) < 0.012 {
			c = add(c, withAlpha(hex(0xffffff), 0.5*(1-math.Abs(inner)/0.012)))
		}
	}
	// The rim: a bright white-cyan edge.
	if math.Abs(d) < 0.022 {
		c = over(c, withAlpha(lerp(hex(0xffffff), hex(0x9ae8ff), (cy+r)/(2*r)), 1-math.Abs(d)/0.022*0.6))
	}
	// Sparkles at the top-left and bottom-right points of light.
	for _, s := range [][3]float64{{-0.2, -0.27, 0.07}, {0.23, 0.24, 0.05}} {
		dx, dy := cx-s[0], cy-s[1]
		arm := math.Min(math.Abs(dx)*math.Hypot(dy, 0.002), math.Abs(dy)*math.Hypot(dx, 0.002))
		if math.Hypot(dx, dy) < s[2] {
			c = add(c, withAlpha(hex(0xffffff), math.Exp(-arm/0.00006)*(1-math.Hypot(dx, dy)/s[2])))
		}
	}
	return c
}

func draw(size int) *image.NRGBA {
	const ss = 4 // supersampling per axis
	img := image.NewNRGBA(image.Rect(0, 0, size, size))
	for py := 0; py < size; py++ {
		for px := 0; px < size; px++ {
			var acc rgba
			for sy := 0; sy < ss; sy++ {
				for sx := 0; sx < ss; sx++ {
					s := sample((float64(px)+(float64(sx)+0.5)/ss)/float64(size),
						(float64(py)+(float64(sy)+0.5)/ss)/float64(size))
					acc.r += s.r
					acc.g += s.g
					acc.b += s.b
				}
			}
			n := float64(ss * ss)
			img.SetNRGBA(px, py, color.NRGBA{uint8(acc.r / n * 255), uint8(acc.g / n * 255),
				uint8(acc.b / n * 255), 255})
		}
	}
	return img
}

func main() {
	dir := "."
	if len(os.Args) > 1 {
		dir = os.Args[1]
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		panic(err)
	}
	for _, out := range []struct {
		name string
		size int
	}{{"apple-touch-icon.png", 180}, {"icon-192.png", 192}, {"icon-512.png", 512}, {"favicon-32.png", 32}} {
		f, err := os.Create(filepath.Join(dir, out.name))
		if err != nil {
			panic(err)
		}
		if err := png.Encode(f, draw(out.size)); err != nil {
			panic(err)
		}
		f.Close()
		fmt.Println(filepath.Join(dir, out.name))
	}
}
