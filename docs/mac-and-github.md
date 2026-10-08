# Mac setup and GitHub publishing

The archive contains source code; your Mac builds its own executables.

## Download and build

Download `zerosum.zip` into your Downloads folder. In Terminal:

```bash
mkdir -p ~/Projects
unzip ~/Downloads/zerosum.zip -d ~/Projects
cd ~/Projects/zerosum
c++ --version
make -j2
python3 -m pip install -r requirements.txt
make test
./build/zerosum solve leduc --algo dcfr --iterations 1000 --every 250
```

If the compiler is missing, run `xcode-select --install`, finish the installer,
and repeat `make -j2`. If Python 3 is missing and you have Homebrew, run
`brew install python`. The Python packages are needed only for the
linear-program check and the report; without them `make test` still runs the
C++ tests and the command-line tests and reports the rest as skipped.

If `pip install` refuses with "externally-managed-environment", create a
virtual environment first:

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

## Publish to GitHub from Terminal

Install the GitHub CLI if `gh --version` does not work:

```bash
brew install gh
```

If Homebrew is not installed, use the official instructions at
https://brew.sh/, then install `gh`.

Authenticate using the interactive terminal flow:

```bash
gh auth login --scopes workflow
gh auth setup-git
```

Choose GitHub.com and HTTPS. The flow offers browser sign-in or token sign-in.
The extra scope permits uploading the included GitHub Actions workflow.

Create and publish the repository:

```bash
cd ~/Projects/zerosum
git init -b main
git add .
git -c user.name='Tom Krane' -c user.email='tkran2@users.noreply.github.com' commit -m 'Build CFR solvers for imperfect-information games with LP verification'
gh repo create zerosum --public --source=. --remote=origin --push
gh repo view --json url --jq .url
```

The command creates a public repository under the account selected by
`gh auth login`. If `zerosum` already exists, choose a different unused
name in the `gh repo create` command. It will not overwrite an existing repo.

## Save later changes

```bash
make test
git add .
git commit -m 'Describe the change you made'
git push
```

Only the initial commit above supplies a name and email for that one command.
If your Mac's Git identity is not configured, configure it before later commits:

```bash
git config user.name 'Tom Krane'
git config user.email 'tkran2@users.noreply.github.com'
```

These settings apply to this repository. The noreply email avoids publishing
your school email in Git commit metadata.
