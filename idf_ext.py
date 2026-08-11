from pathlib import Path
import subprocess
import sys


def action_extensions(base_actions, project_path):
    from idf_py_actions.errors import FatalError

    project_root = Path(project_path).resolve()

    def compile_shaders(action, ctx, args):
        result = subprocess.run(
            [sys.executable, "-m", "tools.shaderc", "--project-root", str(project_root)],
            cwd=project_root,
            check=False,
        )
        if result.returncode != 0:
            raise FatalError("GRAPE shader compilation failed")

    all_action = dict(base_actions.get("actions", {}).get("all", {}))
    if not all_action:
        raise RuntimeError("ESP-IDF 'all' action is unavailable")

    dependencies = list(all_action.get("dependencies", []))
    if "compile-shaders" not in dependencies:
        dependencies.append("compile-shaders")
    all_action["dependencies"] = dependencies

    return {
        "actions": {
            "all": all_action,
            "compile-shaders": {
                "callback": compile_shaders,
                "help": "Compile GRAPE .grsh shaders into generated C and headers.",
            },
        },
    }
