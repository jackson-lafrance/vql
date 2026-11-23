from pathlib import Path
import math

sizes = [1000, 2000, 4000, 8000, 16000, 32000, 64000]
here = Path(__file__).resolve().parent
results = here / "results"


def read_time(path):
    for line in path.read_text().splitlines():
        if line.startswith("WALL TIME (S):"):
            return float(line.split(":", 1)[1])
    raise RuntimeError(f"MISSING WALL TIME IN {path}!!!!")


def slope(points):
    xs = [math.log(n) for n, _ in points]
    ys = [math.log(seconds) for _, seconds in points]
    x_mean = sum(xs) / len(xs)
    y_mean = sum(ys) / len(ys)
    return sum((x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)) / sum(
        (x - x_mean) ** 2 for x in xs
    )


series = {
    "Join": [(n, read_time(results / f"result-{n}.txt")) for n in sizes],
    "Select": [(n, read_time(results / f"selection-{n}.txt")) for n in sizes],
    "Project": [(n, read_time(results / f"projection-{n}.txt")) for n in sizes],
}

width = 1000
height = 650
left = 100
right = 40
top = 60
bottom = 90
plot_width = width - left - right
plot_height = height - top - bottom
x_min = math.log10(min(sizes))
x_max = math.log10(max(sizes))
y_values = [seconds for points in series.values() for _, seconds in points]
y_min = math.floor(math.log10(min(y_values)))
y_max = math.ceil(math.log10(max(y_values)))
colors = {"Join": "#d1495b", "Select": "#00798c", "Project": "#edae49"}


def x_position(n):
    return left + (math.log10(n) - x_min) / (x_max - x_min) * plot_width


def y_position(seconds):
    return top + (y_max - math.log10(seconds)) / (y_max - y_min) * plot_height


svg = [
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
    '<rect width="100%" height="100%" fill="#ffffff"/>',
    '<style>text { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; fill: #202124; } .grid { stroke: #d9dde3; stroke-width: 1; } .axis { stroke: #202124; stroke-width: 2; }</style>',
    f'<text x="{width / 2}" y="32" text-anchor="middle" font-size="22" font-weight="700">Relasaurus Operator Scaling</text>',
]

for exponent in range(y_min, y_max + 1):
    seconds = 10**exponent
    y = y_position(seconds)
    svg.append(f'<line class="grid" x1="{left}" y1="{y:.2f}" x2="{width - right}" y2="{y:.2f}"/>')
    svg.append(f'<text x="{left - 12}" y="{y + 5:.2f}" text-anchor="end" font-size="13">1e{exponent}</text>')

for n in sizes:
    x = x_position(n)
    svg.append(f'<line class="grid" x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{height - bottom}"/>')
    svg.append(f'<text x="{x:.2f}" y="{height - bottom + 24}" text-anchor="middle" font-size="13">{n}</text>')

svg.append(f'<line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{height - bottom}"/>')
svg.append(f'<line class="axis" x1="{left}" y1="{height - bottom}" x2="{width - right}" y2="{height - bottom}"/>')
svg.append(f'<text x="{left + plot_width / 2}" y="{height - 25}" text-anchor="middle" font-size="16">Tuples per input relation (log scale)</text>')
svg.append(f'<text x="24" y="{top + plot_height / 2}" text-anchor="middle" font-size="16" transform="rotate(-90 24 {top + plot_height / 2})">Wall time in seconds (log scale)</text>')

for name, points in series.items():
    color = colors[name]
    coordinates = " ".join(
        f"{x_position(n):.2f},{y_position(seconds):.2f}" for n, seconds in points
    )
    svg.append(f'<polyline points="{coordinates}" fill="none" stroke="{color}" stroke-width="3"/>')
    for n, seconds in points:
        svg.append(f'<circle cx="{x_position(n):.2f}" cy="{y_position(seconds):.2f}" r="5" fill="{color}"/>')

legend_x = left + 20
legend_y = top + 20
svg.append(f'<rect x="{legend_x - 12}" y="{legend_y - 20}" width="230" height="105" rx="8" fill="#ffffff" stroke="#c8cdd4"/>')
for index, (name, points) in enumerate(series.items()):
    y = legend_y + index * 30
    measured_slope = slope(points)
    svg.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 30}" y2="{y}" stroke="{colors[name]}" stroke-width="4"/>')
    svg.append(f'<text x="{legend_x + 42}" y="{y + 5}" font-size="14">{name} (slope {measured_slope:.3f})</text>')

svg.append("</svg>")
output = here / "performance.svg"
output.write_text("\n".join(svg))

for name, points in series.items():
    print(f"{name}: slope={slope(points):.6f}")
print(output)
