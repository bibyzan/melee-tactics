// SPDX-License-Identifier: GPL-3.0-or-later

// Draws Melee Tactics' app icon: a gold ring and a bold M on a fiery
// red-orange field, in Melee's title-screen colours (an original emblem, not
// Nintendo's). Full bleed, so iOS and Android can round or mask the corners
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

func lerp(a, b rgba, t float64) rgba {
	t = math.Max(0, math.Min(1, t))
	return rgba{a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t, a.b + (b.b-a.b)*t, a.a + (b.a-a.a)*t}
}

func hex(v uint32) rgba {
	return rgba{float64(v>>16&0xFF) / 255, float64(v>>8&0xFF) / 255, float64(v&0xFF) / 255, 1}
}

// Distance from p to the segment ab.
func segment(px, py, ax, ay, bx, by float64) float64 {
	dx, dy := bx-ax, by-ay
	t := math.Max(0, math.Min(1, ((px-ax)*dx+(py-ay)*dy)/(dx*dx+dy*dy)))
	return math.Hypot(px-(ax+t*dx), py-(ay+t*dy))
}

// The M: two legs and a V between them, as thick strokes.
func inM(x, y float64) bool {
	const w = 0.042
	legs := [][4]float64{
		{0.32, 0.67, 0.32, 0.34},
		{0.32, 0.34, 0.5, 0.56},
		{0.5, 0.56, 0.68, 0.34},
		{0.68, 0.34, 0.68, 0.67},
	}
	for _, l := range legs {
		if segment(x, y, l[0], l[1], l[2], l[3]) <= w {
			return true
		}
	}
	return false
}

func inRing(x, y float64) bool {
	d := math.Hypot(x-0.5, y-0.5)
	return d >= 0.345 && d <= 0.405
}

// One sample of the icon at (x, y) in [0, 1].
func sample(x, y float64) rgba {
	// The field: hot at the top middle, down to a deep red at the edges.
	d := math.Hypot(x-0.5, (y-0.4)*1.1)
	c := lerp(hex(0xFF9A2A), hex(0xD8261E), d/0.45)
	c = lerp(c, hex(0x3A0608), (d-0.45)/0.4)
	// A soft shadow under the ring and the M.
	const sx, sy = 0.012, 0.02
	if inRing(x-sx, y-sy) || inM(x-sx, y-sy) {
		c = over(c, rgba{0, 0, 0, 0.35})
	}
	// Gold, bright at the top and deeper at the bottom.
	gold := lerp(hex(0xFFF4C8), hex(0xE0A020), (y-0.1)/0.8)
	if inRing(x, y) {
		c = over(c, gold)
	}
	if inM(x, y) {
		c = over(c, lerp(hex(0xFFFFFF), hex(0xFFE6A0), (y-0.3)/0.4))
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
