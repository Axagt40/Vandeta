cd /workspaces && rm -rf scrub && git clone --no-local https://github.com/Axagt40/Vandeta.git scrub && cd scrub && git filter-repo --force --message-callback 'import re
m = message
m = re.sub(rb"\n*(\xf0\x9f\xa4\x96 )?Generated with Codebuff[^\n]*", b"", m)
m = m.replace(b"Co-Authored-By: Codebuff <noreply@codebuff.com>", b"")
m = m.replace(b"FREEBUFF", b"local notes")
m = re.sub(rb"\n{3,}", b"\n\n", m).rstrip() + b"\n"
return m' && git remote add origin https://github.com/Axagt40/Vandeta.git && git push --force origin main && git push --force origin --tags && echo PUSHED; echo '--- CHECK ---'; git log --all --grep=Codebuff --oneline; echo "codebuff body hits: $(git log --all --format=%b | grep -ci codebuff)"; git log --all --grep=FREEBUFF --oneline; echo '--- AUTHORS ---'; git log --all --format='%an <%ae>' | sort -u
