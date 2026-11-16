#!/usr/bin/env python3
"""Sign and verify a release HAP using protected CI credentials."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZipFile

BUNDLE_ID = "com.matterdesk.harmony"
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta|rc)\.[1-9][0-9]*)?")
SECRETS = ("HARMONY_KEYSTORE_B64", "HARMONY_CERT_B64", "HARMONY_PROFILE_B64",
           "HARMONY_KEY_ALIAS", "HARMONY_KEY_PASSWORD", "HARMONY_KEYSTORE_PASSWORD")


def check_hap(path, version, code):
    with ZipFile(path) as archive:
        app = json.loads(archive.read("module.json"))["app"]
        if (app.get("bundleName"), app.get("versionName"), app.get("versionCode")) != (BUNDLE_ID, version, code):
            raise SystemExit("HAP bundle or version does not match the release")
        if app.get("debug") is not False:
            raise SystemExit("A release requires a non-debug HAP")
        if "ets/modules.abc" not in archive.namelist() or "resources/rawfile/camera/player.js" not in archive.namelist():
            raise SystemExit("HAP is missing compiled ArkTS or the shared camera player")


def java_arg(value):
    # Java launcher response file syntax. Passwords never appear in argv.
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise SystemExit("Signing input contains unsupported control characters")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hap", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--version-code", type=int, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true", help="Validate unsigned release packaging without signing")
    args = parser.parse_args()
    if not VERSION.fullmatch(args.version) or not 1 <= args.version_code <= 2147483647:
        parser.error("Invalid Harmony release version or version code")
    check_hap(args.hap, args.version, args.version_code)
    if args.check_only:
        print(f"Harmony HAP metadata verified: {BUNDLE_ID} {args.version} ({args.version_code})")
        return
    credentials = {name: os.environ.pop(name, "") for name in SECRETS}
    missing = [name for name, value in credentials.items() if not value]
    if missing:
        raise SystemExit("Missing protected Harmony signing settings: " + ", ".join(missing))
    studio = Path(os.environ.get("DEVECO_STUDIO_HOME", "/opt/devecostudio"))
    java = Path(os.environ.get("JAVA_HOME", studio / "jbr")) / "bin/java"
    sdk = Path(os.environ.get("DEVECO_SDK_HOME", studio / "sdk"))
    signer = sdk / "default/openharmony/toolchains/lib/hap-sign-tool.jar"
    if not java.is_file() or not signer.is_file():
        raise SystemExit("The release runner requires DevEco API 26 and its JDK")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir.resolve() / f"MatterDesk-{args.version}-harmony.hap"
    with tempfile.TemporaryDirectory(prefix="matterdesk-hap-sign-") as directory:
        work = Path(directory)
        store, cert, profile = (work / name for name in ("key.p12", "app.cer", "profile.p7b"))
        for key, file in zip(SECRETS, (store, cert, profile)):
            file.write_bytes(base64.b64decode("".join(credentials[key].split()), validate=True))
            file.chmod(0o600)
        decoded = subprocess.check_output(["openssl", "cms", "-verify", "-binary", "-inform", "DER",
                                           "-noverify", "-in", str(profile)], stderr=subprocess.DEVNULL)
        provisioning = json.loads(decoded)
        if (provisioning.get("type") != "release"
                or provisioning.get("bundle-info", {}).get("bundle-name") != BUNDLE_ID):
            raise SystemExit("Provisioning profile must be a release profile for com.matterdesk.harmony")
        signed = work / "signed.hap"
        arguments = ["-jar", str(signer), "sign-app", "-mode", "localSign",
                     "-keyAlias", credentials["HARMONY_KEY_ALIAS"],
                     "-keyPwd", credentials["HARMONY_KEY_PASSWORD"],
                     "-keystorePwd", credentials["HARMONY_KEYSTORE_PASSWORD"],
                     "-signAlg", "SHA256withECDSA", "-appCertFile", str(cert),
                     "-profileFile", str(profile), "-inFile", str(args.hap.resolve()),
                     "-keystoreFile", str(store), "-outFile", str(signed), "-compatibleVersion", "26"]
        argfile = work / "sign.args"
        argfile.write_text("\n".join(java_arg(value) for value in arguments), encoding="utf-8")
        argfile.chmod(0o600)
        # Tool output can contain private profile or signing metadata.
        result = subprocess.run([java, "@" + str(argfile)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode or not signed.is_file():
            raise SystemExit("DevEco could not sign the release HAP; check key, certificate and profile")
        argfile.unlink()
        subprocess.run([java, "-jar", signer, "verify-app", "-inFile", signed,
                        "-outCertChain", work / "verified.cer", "-outProfile", work / "verified.p7b"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        if (work / "verified.p7b").read_bytes() != profile.read_bytes():
            raise SystemExit("Verified HAP does not contain the supplied release profile")
        check_hap(signed, args.version, args.version_code)
        output.write_bytes(signed.read_bytes())
    output.with_suffix(".hap.sha256").write_text(f"{hashlib.sha256(output.read_bytes()).hexdigest()}  {output.name}\n")
    print(f"Harmony release HAP signature verified: {output.name}")


if __name__ == "__main__":
    main()
