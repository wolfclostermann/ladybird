import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.dirname(HERE)
REPO = os.path.abspath(os.path.join(ASSETS, "..", "..", ".."))
BIN = os.environ.get(
    "LADYBIRD_BIN_DIR", os.path.join(REPO, "Build", "release", "bin", "Ladybird.app", "Contents", "MacOS")
)

# The scripts write profiles and logs into a work directory and some of them change the extensions they load, so they
# get copies of the fixtures there rather than the ones in the repository.
FIXTURES = {
    "mark-ext": os.path.join(ASSETS, "extensions", "mark-ext"),
    "broken-ext": os.path.join(ASSETS, "extensions", "broken-ext"),
    "nothing-ext": os.path.join(ASSETS, "extensions", "nothing-ext"),
    "site": os.path.join(ASSETS, "sites", "site"),
}


def prepare(work_dir, fresh_profile=True):
    os.makedirs(work_dir, exist_ok=True)
    if fresh_profile:
        for profile in ("profile", "profile4"):
            shutil.rmtree(os.path.join(work_dir, profile), ignore_errors=True)
    for name, source in FIXTURES.items():
        target = os.path.join(work_dir, name)
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(source, target)
