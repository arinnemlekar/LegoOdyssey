"""Coach's tool for managing team accounts and trap keys.

Run on the coach's computer (after `gcloud auth application-default login`):

    python admin_tool.py add-user arin                 # team member
    python admin_tool.py add-user coach --admin        # coach (can delete photos)
    python admin_tool.py reset-password arin
    python admin_tool.py remove-user arin
    python admin_tool.py list-users
    python admin_tool.py add-trap trap-1               # prints the key for config.h
    python admin_tool.py upgrade-photos                # once, after adding the Trash feature

Kids log in with just a username (like "arin") — no email address needed.
Behind the scenes it becomes arin@team75368.example (must match web/js/common.js).
"""

import argparse
import hashlib
import json
import os
import secrets
import sys
from pathlib import Path

import firebase_admin
from firebase_admin import auth, firestore

USERNAME_DOMAIN = "team75368.example"

WORDS = """acorn badger beaver birch bobcat brook canyon cedar comet coral cricket crow
deer delta dune eagle elk falcon fern finch forest fox frost glacier grove hawk
heron hollow island jay juniper kestrel lake lark lichen lynx maple marsh meadow
mink moose moss newt oak orbit otter owl pebble pine plover pond prairie quail
rabbit raven reed ridge river robin rocket sage salmon sparrow spruce star stone
summit swan thrush tide trail trout tundra valley vole willow wolf wren""".split()


def find_project():
    """Use --project, or the project in ../.firebaserc."""
    rc = Path(__file__).resolve().parent.parent / ".firebaserc"
    if rc.exists():
        return json.loads(rc.read_text())["projects"]["default"]
    return None


def make_password():
    return "-".join(secrets.choice(WORDS) for _ in range(3)) + str(secrets.randbelow(90) + 10)


def to_email(username):
    return username if "@" in username else f"{username.lower()}@{USERNAME_DOMAIN}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", help="Firebase project ID (default: from .firebaserc)")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("add-user"); p.add_argument("username"); p.add_argument("--admin", action="store_true")
    p = sub.add_parser("reset-password"); p.add_argument("username")
    p = sub.add_parser("make-admin"); p.add_argument("username")
    p = sub.add_parser("remove-admin"); p.add_argument("username")
    p = sub.add_parser("remove-user"); p.add_argument("username")
    sub.add_parser("list-users")
    p = sub.add_parser("add-trap"); p.add_argument("device_id")
    sub.add_parser("upgrade-photos", help="give older photos the Trash fields (run once)")
    args = parser.parse_args()

    project = args.project or os.environ.get("GCLOUD_PROJECT") or find_project()
    if not project or project == "YOUR_FIREBASE_PROJECT_ID":
        sys.exit("Set your Firebase project ID in .firebaserc (or use --project).")
    if os.environ.get("FIRESTORE_EMULATOR_HOST"):
        # Local testing with the Firebase emulators: no real Google login needed.
        from google.auth.credentials import AnonymousCredentials

        class EmulatorCredential(firebase_admin.credentials.Base):
            def get_credential(self):
                return AnonymousCredentials()

        firebase_admin.initialize_app(EmulatorCredential(), {"projectId": project})
    else:
        firebase_admin.initialize_app(options={"projectId": project})

    if args.command == "add-user":
        password = make_password()
        user = auth.create_user(email=to_email(args.username), password=password, display_name=args.username)
        if args.admin:
            auth.set_custom_user_claims(user.uid, {"role": "admin"})
        print(f"Created {'ADMIN ' if args.admin else ''}user '{args.username}'  password: {password}")

    elif args.command == "reset-password":
        password = make_password()
        user = auth.get_user_by_email(to_email(args.username))
        auth.update_user(user.uid, password=password)
        print(f"New password for '{args.username}': {password}")

    elif args.command in ("make-admin", "remove-admin"):
        user = auth.get_user_by_email(to_email(args.username))
        auth.set_custom_user_claims(user.uid, {"role": "admin"} if args.command == "make-admin" else None)
        print(f"Done. '{args.username}' must log out and back in for this to take effect.")

    elif args.command == "remove-user":
        auth.delete_user(auth.get_user_by_email(to_email(args.username)).uid)
        print(f"Removed '{args.username}'.")

    elif args.command == "list-users":
        for user in auth.list_users().iterate_all():
            role = (user.custom_claims or {}).get("role", "member")
            name = user.email.replace(f"@{USERNAME_DOMAIN}", "")
            print(f"{name:20s} {role}")

    elif args.command == "add-trap":
        device = args.device_id
        key = secrets.token_urlsafe(24)
        db = firestore.client()
        db.collection("trapKeys").document(device).set({"keyHash": hashlib.sha256(key.encode()).hexdigest()})
        trap = db.collection("traps").document(device)
        if not trap.get().exists:
            trap.set({"name": device})
        print(f"Trap '{device}' is ready. Put this in its config.h:\n")
        print(f'#define DEVICE_ID  "{device}"')
        print(f'#define TRAP_KEY   "{key}"')
        print("\nRunning add-trap again makes a NEW key (the old one stops working).")

    elif args.command == "upgrade-photos":
        upgrade_photos()


def upgrade_photos():
    """Photos uploaded before the Trash feature have no 'trashed' field, so the Photos
    page can't find them. Give them one: photos with no animal go to the Trash."""
    db = firestore.client()
    changed = 0
    for snap in db.collection("sightings").stream():
        data = snap.to_dict()
        if "trashed" in data:
            continue
        no_animal = not data.get("isAnimal", True)
        snap.reference.update({
            "trashed": no_animal,
            "trashReason": f"AI: {data.get('label')}" if no_animal else None,
            "trashedBy": "auto" if no_animal else None,
            "trashedAt": firestore.SERVER_TIMESTAMP if no_animal else None,
        })
        changed += 1
    print(f"Upgraded {changed} photos.")


if __name__ == "__main__":
    main()
