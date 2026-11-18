"""Runtime resources consumed by the Cubism 5 renderers."""
from pathlib import Path


def shader_paths(platform: str) -> list[Path]:
    contracts = Path(__file__).resolve().parent
    if platform == "windows":
        return [Path("FrameworkShaders") / name for name in ("CubismEffect.fx", "CubismBlendMode.fx")]
    if platform == "linux":
        names = (contracts / "cubism_opengl_shaders.txt").read_text(encoding="utf-8").splitlines()
        return [Path("FrameworkShaders") / name for name in names]

    if platform == "macos":
        base, fragments, colors, alphas = (
            line.split() for line in (contracts / "cubism_metal_shaders.txt").read_text(encoding="utf-8").splitlines()
        )
        variants = [f"{fragment}{color}{alpha}"
                    for fragment in fragments for color in colors for alpha in alphas
                    if (color, alpha) != ("Normal", "Over")]
        return [Path("FrameworkMetallibs") / f"{name}.metallib" for name in base + variants]
    return []
