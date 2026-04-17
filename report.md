# PES-VCS Lab Report
## OS Unit 4 — Orange Problem 2: Building a Version Control System from Scratch

**Course:** Operating Systems  
**Lab:** Unit 4 — Orange Problem 2  
**Platform:** Ubuntu 22.04  
**Project:** PES-VCS — A Version Control System  

---

## Table of Contents

1. [Overview](#overview)
2. [Phase 1: Object Store — Screenshots & Description](#phase-1-object-store)
3. [Phase 2: Tree Objects — Screenshots & Description](#phase-2-tree-objects)
4. [Phase 3: Staging Area (Index) — Screenshots & Description](#phase-3-staging-area)
5. [Phase 4: Commits and History — Screenshots & Description](#phase-4-commits-and-history)
6. [Final Integration Test](#final-integration-test)
7. [Phase 5: Analysis — Branching and Checkout](#phase-5-branching-and-checkout)
8. [Phase 6: Analysis — Garbage Collection](#phase-6-garbage-collection)

---

## Overview

This lab involved building PES-VCS, a local version control system modeled after Git. The system tracks file changes, stores snapshots using a content-addressable object store, and supports commit history through a linked structure on disk. All four implementation phases — object storage, tree serialization, staging area, and commit creation — were completed along with written analysis of branching and garbage collection.

---

## Phase 1: Object Store

**Files implemented:** `object.c` (`object_write`, `object_read`)

**Concepts covered:** SHA-256 content-addressable storage, file I/O, sharded directory structure

The object store saves every file by its SHA-256 hash. The first two characters of the hash form a subdirectory (e.g., `.pes/objects/ab/`), and the remaining characters form the filename. This sharding prevents any single directory from having too many entries, mirroring Git's design.

### Screenshot 1A — `./test_objects` passing

> ![alt text](<Screenshot 2026-04-17 at 17.33.08.png>)

### Screenshot 1B — Object store directory structure

> ![alt text](<Screenshot 2026-04-17 at 17.33.25.png>)

---

## Phase 2: Tree Objects

**Files implemented:** `tree.c` (`tree_from_index`)

**Concepts covered:** Directory representation as tree objects, binary serialization, recursive structures

A tree object represents a directory snapshot. Each entry in the tree contains a mode (file permissions), a blob or sub-tree hash, and a filename. The `tree_from_index` function builds a tree object from the current staged files in the index.

### Screenshot 2A — `./test_tree` passing

> ![alt text](<Screenshot 2026-04-17 at 17.39.29.png>)

### Screenshot 2B — `xxd` of a raw tree object

> ![alt text](<Screenshot 2026-04-17 at 17.56.41.png>)

---

## Phase 3: Staging Area (Index)

**Files implemented:** `index.c` (`index_load`, `index_save`, `index_add`)

**Concepts covered:** File format design, atomic writes using temp-file + rename, change detection via metadata (mtime, size)

The index is a text-based file at `.pes/index`. Each line stores: `<mode> <hash-hex> <mtime> <size> <path>`. Writing is done atomically — first writing to a temporary file, calling `fsync()`, then renaming into place — to prevent corruption from crashes mid-write.

### Screenshot 3A — `pes init` → `pes add` → `pes status` sequence

> ![alt text](<Screenshot 2026-04-17 at 17.57.10.png>)
```
./pes init
echo "hello" > file1.txt
echo "world" > file2.txt
./pes add file1.txt file2.txt
./pes status
```

### Screenshot 3B — `cat .pes/index`

> ![alt text](<Screenshot 2026-04-17 at 17.57.25.png>)

---

## Phase 4: Commits and History

**Files implemented:** `commit.c` (`commit_create`)

**Concepts covered:** Linked structures on disk, reference files, atomic pointer updates, HEAD management

A commit object contains: a pointer to a tree (the snapshot), an optional parent commit hash, an author string, a timestamp, and a commit message. `commit_create` builds a tree from the index, reads the current HEAD as parent, writes the commit object, then atomically updates HEAD.

The commit chain forms a singly linked list on disk: each commit points back to its parent, creating a traversable history.

### Screenshot 4A — `./pes log` showing three commits

> ![alt text](<Screenshot 2026-04-17 at 17.57.46.png>)
```
./pes init
echo "Hello" > hello.txt
./pes add hello.txt
./pes commit -m "Initial commit"

echo "World" >> hello.txt
./pes add hello.txt
./pes commit -m "Add world"

echo "Goodbye" > bye.txt
./pes add bye.txt
./pes commit -m "Add farewell"

./pes log
```

### Screenshot 4B — `find .pes -type f | sort` showing object growth

> ![alt text](<Screenshot 2026-04-17 at 17.58.07.png>)

### Screenshot 4C — `cat .pes/refs/heads/main` and `cat .pes/HEAD`

> ![alt text](<Screenshot 2026-04-17 at 17.58.22.png>)

---

## Final Integration Test

### Screenshot — `make test-integration` passing

> ![alt text](<Screenshot 2026-04-17 at 18.00.22.png>)
> ![alt text](<Screenshot 2026-04-17 at 18.00.37.png>)

---

## Phase 5: Branching and Checkout

### Q5.1 — How would you implement `pes checkout <branch>`?

To implement `pes checkout <branch>`, the following changes must occur in the `.pes/` directory:

**Step 1 — Resolve the branch:** Read `.pes/refs/heads/<branch>` to get the target commit hash.

**Step 2 — Update HEAD:** Overwrite `.pes/HEAD` with `ref: refs/heads/<branch>`. This makes HEAD a symbolic reference pointing to the new branch.

**Step 3 — Walk the target tree:** Read the target commit object, extract its tree hash, then recursively walk the tree to enumerate all files and their corresponding blob hashes.

**Step 4 — Update the working directory:** For each file in the target tree, read its blob from the object store and write it to disk at the correct path. Files present in the current branch but absent from the target tree must be deleted from disk.

**Step 5 — Update the index:** Replace all entries in `.pes/index` with the entries from the target tree (mode, hash, mtime, size, path), reflecting the new branch's staged state.

**What makes this operation complex:**

- Files must be handled recursively across nested subdirectories.
- Files present in the old branch but not the new branch must be deleted — but only if they are tracked (untracked files must be left alone).
- If the user has uncommitted local changes to a file that also differs between branches, checkout must refuse rather than silently overwriting their work.
- The `HEAD` update must happen last (atomically) — if any earlier step fails, HEAD should not have moved.
- The case where the target branch does not exist must be handled gracefully.

---

### Q5.2 — How do you detect a "dirty working directory" conflict?

Using only the index and the object store, a dirty working directory can be detected as follows:

**Step 1 — Detect modified tracked files:**  
For each entry in `.pes/index`, compare the on-disk file's current `mtime` and `size` against the values stored in the index entry. If either differs, the file is *potentially* modified. To confirm, read the actual file content, compute its SHA-256 hash, and compare against the blob hash stored in the index. If the hashes differ, the file is **locally modified (dirty)**.

**Step 2 — Identify conflict with target branch:**  
For each locally modified file, check whether the blob hash in the current index differs from the blob hash for the same filename in the target branch's tree. If it does, a conflict exists — the file has been changed locally AND would be overwritten by the checkout.

**Step 3 — Refuse if conflict found:**  
If any such conflicting file is found, abort the checkout and print an error:
```
error: Your local changes to 'hello.txt' would be overwritten by checkout.
Please commit or stash your changes before switching branches.
```

This approach requires only the index (for mtime/size/hash of tracked files) and the object store (for reading blobs and target tree structures) — no diff tools or extra metadata files needed.

---

### Q5.3 — What is "Detached HEAD" and how do you recover commits made in that state?

**What Detached HEAD means:**  
Normally, `.pes/HEAD` contains a symbolic reference like `ref: refs/heads/main`. In detached HEAD state, `.pes/HEAD` contains a raw commit hash directly (e.g., `a1b2c3d4...`). This means no branch is active.

**What happens when you commit in detached HEAD state:**  
New commit objects are created in the object store and HEAD is updated to point to each new commit hash directly. However, no branch reference file under `.pes/refs/heads/` is updated. The commits are "orphaned" — reachable only by following HEAD, not through any named branch.

**The problem:** As soon as you switch to another branch, HEAD is overwritten to point to that branch. The orphaned commits are now unreachable. They will eventually be deleted by garbage collection.

**How to recover:**

*Before switching away (safest):*
```bash
# Create a new branch pointing to the current HEAD commit
# Equivalent to writing HEAD's hash into .pes/refs/heads/recovery-branch
git branch recovery-branch
```

*After switching away:*  
Use `git reflog` (Git's log of all HEAD movements) to find the last known commit hash from the detached session. Then create a branch pointing to it:
```bash
git reflog                          # Find the orphaned commit hash
git branch recovery-branch <hash>   # Resurrect it as a named branch
```

In PES-VCS (which does not implement reflog), recovery would require scanning all objects in the store for commit objects not reachable from any branch — a manual mark-and-sweep in reverse.

---

## Phase 6: Garbage Collection and Space Reclamation

### Q6.1 — Algorithm to find and delete unreachable objects

**Algorithm: Mark and Sweep**

**Mark Phase:**

1. Initialize an empty hash set `reachable` (use an unordered hash set for O(1) insert and lookup).
2. For each file in `.pes/refs/heads/`, read the commit hash it contains. Add it to `reachable`.
3. For each starting commit hash, perform a depth-first traversal:
   - Read the commit object. Add its hash to `reachable`.
   - If the commit has a parent hash, recurse into it (if not already visited).
   - Read the commit's tree hash. Add it to `reachable`. Recursively walk the tree:
     - For each entry: if it is a blob, add its hash to `reachable`. If it is a sub-tree, recurse.

**Sweep Phase:**

4. List all files under `.pes/objects/` by traversing the two-level sharded directory structure.
5. For each file, reconstruct the full hash (directory prefix + filename).
6. If the hash is **not** in the `reachable` set, delete the file.

**Data structure:** An unordered hash set (e.g., a hash table or a `bool` array keyed by truncated hash prefix) for O(1) average-case insert and lookup. A simple sorted array with binary search also works for smaller repositories.

**Estimate for 100,000 commits, 50 branches:**  
Assuming each commit references on average 1 tree and ~10 blobs, with unchanged files shared across commits, a rough upper bound of unique objects is:
- Commit objects: ~100,000
- Tree objects: ~100,000 (one per commit)
- Blob objects: ~500,000 (many shared across commits)
- **Total to visit: ~700,000 – 1,000,000 objects**

Starting from 50 branch tips and traversing full histories, the mark phase visits all reachable objects exactly once. The sweep phase also visits all objects in the store once. Total: approximately **1–2 million object reads** in the worst case.

---

### Q6.2 — Race condition between GC and a concurrent commit

**The Race Condition:**

Consider the following interleaving between a `commit` operation and a garbage collection (GC) run happening concurrently:

1. The `commit` operation writes a new **blob object** (hash `abc123`) for a file being staged. The blob is now on disk in `.pes/objects/ab/c123...`. However, the commit object referencing this blob has **not yet been written**.

2. At this exact moment, GC begins its **mark phase**. It traverses all branches and their commit histories. Since no commit object yet references `abc123`, GC classifies it as **unreachable**.

3. GC proceeds to the **sweep phase** and **deletes** the file at `.pes/objects/ab/c123...`.

4. The `commit` operation now writes the commit object, which references the tree, which references blob `abc123`. The repository now points to a **non-existent object** — it is **corrupted**. Any future checkout or log operation that tries to read `abc123` will fail.

**How Git's real GC avoids this:**

- **Grace period:** Git's GC (via `git gc` and `git prune`) never deletes loose objects newer than **2 weeks** by default, regardless of reachability. This gives any in-progress operation more than enough time to complete and reference the new objects.

- **Atomic object creation:** Git writes each new object to a temporary file first, then renames it into its final path atomically (using `rename(2)`). A partially written object is never at the canonical hash path, so GC's sweep never sees a half-written file.

- **Lock files:** Git uses `.lock` files to serialize critical operations. A `gc.pid` file prevents two GC processes from running simultaneously and racing against each other.

- **Reference transaction log (reflog):** Even if an object becomes temporarily unreachable (e.g., due to a reset or rebase), the reflog keeps the old commit hashes alive and prevents them from being pruned until the reflog entries expire.

Together, these mechanisms ensure that GC is safe to run even on an active repository.

---

## Submission Checklist

| Item | Status |
|------|--------|
| Screenshot 1A — `./test_objects` passing | ☐ Insert screenshot |
| Screenshot 1B — `find .pes/objects -type f` | ☐ Insert screenshot |
| Screenshot 2A — `./test_tree` passing | ☐ Insert screenshot |
| Screenshot 2B — `xxd` of raw tree object | ☐ Insert screenshot |
| Screenshot 3A — `pes init` → `pes add` → `pes status` | ☐ Insert screenshot |
| Screenshot 3B — `cat .pes/index` | ☐ Insert screenshot |
| Screenshot 4A — `./pes log` three commits | ☐ Insert screenshot |
| Screenshot 4B — `find .pes -type f \| sort` | ☐ Insert screenshot |
| Screenshot 4C — `cat .pes/refs/heads/main` + `cat .pes/HEAD` | ☐ Insert screenshot |
| Final — `make test-integration` | ☐ Insert screenshot |
| `object.c` implemented | ☐ |
| `tree.c` implemented | ☐ |
| `index.c` implemented | ☐ |
| `commit.c` implemented | ☐ |
| Q5.1, Q5.2, Q5.3 answered | ✅ |
| Q6.1, Q6.2 answered | ✅ |
| Minimum 5 commits per phase | ☐ |
| Repository is public | ☐ |
| Report placed at root of repo | ☐ |
