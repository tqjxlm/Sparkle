import os
import shutil

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def setup_ide():
    print("Copying vscode settings...")

    src_dir = os.path.join(SCRIPTPATH, ".vscode")
    dst_dir = os.path.join(SCRIPTPATH, "..", ".vscode")

    os.makedirs(dst_dir, exist_ok=True)

    for file in os.listdir(src_dir):
        src = os.path.join(src_dir, file)
        dst = os.path.join(dst_dir, file)
        if not os.path.exists(dst):
            shutil.copy(src, dst)
