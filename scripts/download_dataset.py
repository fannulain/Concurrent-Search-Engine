#!/usr/bin/env python3
"""Download English plain-text books from Project Gutenberg and strip license boilerplate."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import sys
import urllib.error
import urllib.request

START_MARKERS = ("*** START OF", "***START OF")
END_MARKERS = ("*** END OF", "***END OF")


def strip_gutenberg_boilerplate(text: str) -> str:
    start_idx = 0
    header_slice = text[:32768]
    for marker in START_MARKERS:
        pos = header_slice.find(marker)
        if pos != -1:
            nl = text.find("\n", pos)
            if nl != -1:
                start_idx = nl + 1
            break

    end_idx = len(text)
    footer_start = max(0, len(text) - 65536)
    footer_slice = text[footer_start:]
    for marker in END_MARKERS:
        pos = footer_slice.rfind(marker)
        if pos != -1:
            abs_pos = footer_start + pos
            nl = text.rfind("\n", 0, abs_pos)
            candidate = nl + 1 if nl != -1 else abs_pos
            if candidate > start_idx:
                end_idx = candidate
            break

    return text[start_idx:end_idx].strip() + "\n"


def fetch_book(book_id: int, timeout: float = 15.0) -> tuple[int, str | None]:
    urls = [
        f"https://www.gutenberg.org/cache/epub/{book_id}/pg{book_id}.txt",
        f"https://www.gutenberg.org/files/{book_id}/{book_id}-0.txt",
        f"https://www.gutenberg.org/files/{book_id}/{book_id}.txt",
    ]
    headers = {"User-Agent": "ConcurrentSearchEngineDatasetLoader/1.0"}

    for url in urls:
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                if resp.status != 200:
                    continue
                raw = resp.read().decode("utf-8", errors="ignore")
                if len(raw) < 2048:
                    continue
                return book_id, strip_gutenberg_boilerplate(raw)
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError):
            continue

    return book_id, None


def current_dataset_bytes(output_dir: Path) -> tuple[int, int]:
    total_bytes = 0
    file_count = 0
    for path in output_dir.rglob("*.txt"):
        if path.is_file():
            total_bytes += path.stat().st_size
            file_count += 1
    return file_count, total_bytes


def main() -> int:
    parser = argparse.ArgumentParser(description="Download Project Gutenberg English text dataset")
    parser.add_argument("--output", default="data", help="Destination directory (default: data)")
    parser.add_argument("--target-gb", type=float, default=1.5, help="Target dataset size in GB (default: 1.5)")
    parser.add_argument("--start-id", type=int, default=1, help="Starting Gutenberg ebook ID (default: 1)")
    parser.add_argument("--max-books", type=int, default=5000, help="Maximum ebook IDs to scan (default: 5000)")
    parser.add_argument("--workers", type=int, default=4, help="Concurrent download threads (default: 4)")
    args = parser.parse_args()

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)
    target_bytes = int(args.target_gb * (1024**3))

    existing_count, total_bytes = current_dataset_bytes(output_dir)
    print(f"Existing dataset: {existing_count} files, {total_bytes / (1024**3):.3f} GB")

    if total_bytes >= target_bytes:
        print("Target dataset size already satisfied.")
        return 0

    candidate_ids = [
        book_id
        for book_id in range(args.start_id, args.start_id + args.max_books)
        if not (output_dir / f"{book_id}.txt").exists()
    ]

    downloaded = 0
    batch_size = max(args.workers * 4, 16)

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        for i in range(0, len(candidate_ids), batch_size):
            if total_bytes >= target_bytes:
                break
            batch = candidate_ids[i : i + batch_size]
            futures = [executor.submit(fetch_book, bid) for bid in batch]
            for future in as_completed(futures):
                book_id, text = future.result()
                if text is None:
                    continue
                target_file = output_dir / f"{book_id}.txt"
                target_file.write_text(text, encoding="utf-8")
                total_bytes += os.path.getsize(target_file)
                downloaded += 1
                if downloaded % 25 == 0 or total_bytes >= target_bytes:
                    print(
                        f"Downloaded +{downloaded} files | Total: {total_bytes / (1024**2):.1f} MB "
                        f"({total_bytes / (1024**3):.3f} GB)"
                    )
                if total_bytes >= target_bytes:
                    break

    final_count, final_bytes = current_dataset_bytes(output_dir)
    print(f"Finished: {final_count} files, {final_bytes / (1024**3):.3f} GB")
    return 0 if final_bytes > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
