#!/usr/bin/env python

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

from __future__ import print_function

import hashlib
import logging
import os
import shutil
import subprocess
import sys
import tempfile
try:
  import urllib.request as urllib
except ImportError:
  import urllib

from kudu_util import check_output, confirm_prompt, Colors, get_my_email, get_upstream_commit, \
  init_logging, ROOT


def check_repo_not_dirty():
  """Check that the git repository isn't dirty."""
  dirty_repo = subprocess.call("git diff --quiet && git diff --cached --quiet",
                               shell=True) != 0
  if not dirty_repo:
    return
  print("The repository does not appear to be clean.")
  print(Colors.RED + "The source release will not include your local changes." + Colors.RESET)
  if not confirm_prompt("Continue?"):
    sys.exit(1)


def check_no_local_commits():
  """
  Check that there are no local commits which haven't been pushed to the upstream
  repo via Jenkins.
  """
  upstream_commit = get_upstream_commit()
  cur_commit = check_output(["git", "rev-parse", "HEAD"]).strip().decode('utf-8')

  if upstream_commit == cur_commit:
    return
  print("The repository appears to have local commits:")
  subprocess.check_call(["git", "log", "--oneline", "%s..HEAD" % upstream_commit])

  print(Colors.RED + "This should not be an official release!" + Colors.RESET)
  if not confirm_prompt("Continue?"):
    sys.exit(1)


def get_version_number():
  """ Return the current version number of Kudu. """
  return open(os.path.join(ROOT, "version.txt")).read().strip()


def create_tarball():
  artifact_name = "apache-kudu-%s" % get_version_number()
  build_dir = os.path.join(ROOT, "build")
  if not os.path.exists(build_dir):
    os.path.makedirs(build_dir)
  tarball_path = os.path.join(build_dir, artifact_name + ".tar.gz")
  print("Exporting source tarball...")
  subprocess.check_output(["git", "archive",
                           "--prefix=%s/" % artifact_name,
                           "--output=%s" % tarball_path,
                           "HEAD"])
  print(Colors.GREEN + "Generated tarball:\t" + Colors.RESET + tarball_path)
  return tarball_path


def sign_tarball(tarball_path):
  """ Prompt the user to GPG-sign the tarball using their Apache GPG key. """
  if not confirm_prompt("Would you like to GPG-sign the tarball now?"):
    return

  email = get_my_email()
  if not email.endswith("@apache.org"):
    print(Colors.YELLOW, "Your email address for the repository is not an @apache.org address.")
    print("Release signatures should typically be signed by committers with @apache.org GPG keys.")
    print(end=Colors.RESET)
    if not confirm_prompt("Continue?"):
      return

  try:
    subprocess.check_call(["gpg", "--detach-sign", "--armor", "-u", email, tarball_path])
  except subprocess.CalledProcessError:
    print(Colors.RED + "GPG signing failed. Artifact will not be signed." + Colors.RESET)
    return
  print(Colors.GREEN + "Generated signature:\t" + Colors.RESET, tarball_path + ".asc")


def checksum_file(summer, path):
  """
  Calculates the checksum of the file 'path' using the provided hashlib
  digest implementation. Returns the hex form of the digest.
  """
  with open(path, "rb") as f:
    # Read the file in 4KB chunks until EOF.
    while True:
      chunk = f.read(4096)
      if not chunk:
        break
      summer.update(chunk)
  return summer.hexdigest()


def gen_sha_file(tarball_path):
  """
  Create a sha checksum file of the tarball.

  The output format is compatible with command line tools like 'sha512sum' so it
  can be used to verify the checksum.
  """
  digest = checksum_file(hashlib.sha512(), tarball_path)
  path = tarball_path + ".sha512"
  with open(path, "w") as f:
      f.write("%s  %s\n" % (digest, os.path.basename(tarball_path)))
  print(Colors.GREEN + "Generated sha:\t\t" + Colors.RESET + path)


def run_rat(tarball_path):
  """
  Run Apache RAT on the source tarball.

  Raises an exception on failure.
  """
  if not confirm_prompt("Would you like to run Apache RAT (Release Audit Tool) now?"):
    return

  # TODO: Cache and call the jar from the maven repo?
  rat_url = "http://central.maven.org/maven2/org/apache/rat/apache-rat/0.12/apache-rat-0.12.jar"

  tmpdir_path = tempfile.mkdtemp()
  rat_report_result = ''
  try:
    rat_jar_dest = "%s/%s" % (tmpdir_path, os.path.basename(rat_url))

    print("> Downloading RAT jar from " + rat_url)
    urllib.urlretrieve(rat_url, rat_jar_dest)

    print("> Running RAT...")
    xml = subprocess.check_output(["java", "-jar", rat_jar_dest, "-x", tarball_path])
    rat_report_dest = "%s/%s" % (tmpdir_path, "rat_report.xml")
    with open(rat_report_dest, "wb") as f:
        f.write(xml)

    print("> Parsing RAT report...")
    rat_report_result = subprocess.check_output(
        ["./build-support/release/check-rat-report.py",
         "./build-support/release/rat_exclude_files.txt",
         rat_report_dest],
        stderr=subprocess.STDOUT)
    print(Colors.GREEN + "RAT: LICENSES APPROVED" + Colors.RESET)
  except subprocess.CalledProcessError as e:
    print(Colors.RED + "RAT: LICENSES NOT APPROVED" + Colors.RESET)
    print(e.output)
    raise e
  finally:
    shutil.rmtree(tmpdir_path)

def main():
  # Change into the source repo so that we can run git commands without having to
  # specify cwd=BUILD_SUPPORT every time.
  os.chdir(ROOT)
  check_repo_not_dirty()
  check_no_local_commits()
  tarball_path = create_tarball()
  gen_sha_file(tarball_path)
  sign_tarball(tarball_path)
  run_rat(tarball_path)

  print(Colors.GREEN + "Release successfully generated!" + Colors.RESET)
  print


if __name__ == "__main__":
  init_logging()
  main()
