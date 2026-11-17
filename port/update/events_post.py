"""Send one batch of the event log (the launcher's events_upload).

    python events_post.py URL BATCH_JSON

Exit 0: the receiver took it. Exit 2: the receiver refused it for good
(400/413), drop the batch. Exit 1: no connection, rate limit, server error:
try again on a later start.
"""
import sys
import urllib.error
import urllib.request


def main():
    url, path = sys.argv[1], sys.argv[2]
    with open(path, "rb") as f:
        body = f.read()
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json", "User-Agent": "hercules-recompiled"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return 0 if r.status == 200 else 1
    except urllib.error.HTTPError as e:
        return 2 if e.code in (400, 413) else 1
    except Exception:
        return 1


if __name__ == "__main__":
    sys.exit(main())
