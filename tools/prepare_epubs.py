import os
import sys
import zipfile
import shutil
import re
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Optional, Tuple, List
from PIL import Image
from io import BytesIO
import argparse
import struct
import itertools
from concurrent.futures import ProcessPoolExecutor

SDCARD_BOOKS_ROOT = "/sdcard/books"

def count_chapter_length(data: bytes) -> int:
    """
    Replicates the C++ chapterLengthCallback logic to calculate text length.
    """
    length = 0
    in_tag = False
    last_space = True
    current_tag = bytearray()
    
    i = 0
    n = len(data)
    while i < n:
        c = data[i]
        
        if c == 60: # '<'
            in_tag = True
            current_tag = bytearray()
            i += 1
            continue
            
        if in_tag:
            if c == 62: # '>'
                in_tag = False
                # Process tag
                try:
                    tag_str = current_tag.decode('utf-8', errors='ignore').lower()
                except:
                    tag_str = ""
                
                is_block = False
                if tag_str in ["p", "/p"] or tag_str.startswith("p ") or tag_str.startswith("/p "): is_block = True
                elif tag_str in ["div", "/div"] or tag_str.startswith("div ") or tag_str.startswith("/div "): is_block = True
                elif tag_str in ["br", "br/"] or tag_str.startswith("br "): is_block = True
                elif tag_str in ["li", "/li"]: is_block = True
                elif len(tag_str) >= 2 and (tag_str[0] == 'h' or (tag_str[0] == '/' and tag_str[1] == 'h')): is_block = True
                
                if is_block:
                    if not last_space:
                        length += 1
                        last_space = True
                elif tag_str == "img" or tag_str.startswith("img "):
                    length += 7 # [Image]
                    last_space = False
            else:
                current_tag.append(c)
            i += 1
            continue
            
        # Outside tag
        if c == 10 or c == 13: # \n or \r
            c = 32 # space
            
        if c == 32:
            if not last_space:
                length += 1
                last_space = True
        else:
            length += 1
            last_space = False
            
        i += 1
        
    return length

def generate_metrics(epub_path: Path) -> bool:
    """
    Generates the m_<filename>.bin metrics file for the given EPUB.
    Returns True if successful.
    """
    metrics_filename = f"m_{epub_path.stem}.bin"
    metrics_path = epub_path.parent / metrics_filename
    
    if metrics_path.exists():
        # print(f"Metrics already exist for {epub_path.name}")
        return True

    print(f"Generating metrics for {epub_path.name}...")
    
    try:
        with zipfile.ZipFile(epub_path, "r") as zf:
            # 1. Find OPF
            try:
                container_data = zf.read("META-INF/container.xml")
                container_root = ET.fromstring(container_data)
                ns_container = {"c": "urn:oasis:names:tc:opendocument:xmlns:container"}
                rootfile_elem = container_root.find(".//c:rootfile", ns_container)
                opf_path = rootfile_elem.get("full-path")
                opf_data = zf.read(opf_path)
                opf_root = ET.fromstring(opf_data)
            except Exception as e:
                print(f"[ERROR] Failed to parse OPF for {epub_path.name}: {e}")
                return False

            ns = {
                "dc": "http://purl.org/dc/elements/1.1/",
                "opf": "http://www.idpf.org/2007/opf",
            }
            
            # 2. Get Manifest and Spine
            manifest = {} # id -> href
            for item in opf_root.findall(".//opf:item", ns):
                manifest[item.get("id")] = item.get("href")
                
            spine_refs = []
            for itemref in opf_root.findall(".//opf:itemref", ns):
                spine_refs.append(itemref.get("idref"))
                
            # 3. Calculate metrics
            chapter_offsets = []
            cumulative_length = 0
            chapter_offsets.append(0) # Start at 0
            
            opf_dir = os.path.dirname(opf_path)
            
            for idref in spine_refs:
                if idref not in manifest:
                    continue
                    
                href = manifest[idref]
                # Resolve path relative to OPF
                full_path = href
                if opf_dir:
                    full_path = opf_dir + "/" + href
                full_path = full_path.replace("\\", "/")
                
                try:
                    content = zf.read(full_path)
                    length = count_chapter_length(content)
                    cumulative_length += length
                    chapter_offsets.append(cumulative_length)
                except KeyError:
                    print(f"[WARN] Chapter file missing: {full_path}")
                    # Add 0 length
                    chapter_offsets.append(cumulative_length)
                except Exception as e:
                    print(f"[WARN] Error reading chapter {full_path}: {e}")
                    chapter_offsets.append(cumulative_length)

            # 4. Save to .bin file
            # Format: Version(1) | TotalChars(4) | Count(4) | Offsets...
            # Note: chapter_offsets contains (Count + 1) entries (0 to Total).
            # The C++ code expects:
            # fwrite(&totalChars, sizeof(size_t), 1, f);
            # fwrite(&count, sizeof(uint32_t), 1, f);
            # fwrite(chapterOffsets.data(), sizeof(size_t), count, f);
            # Wait, C++ `chapterOffsets` vector in `saveBookMetrics` seems to be the sums?
            # In `gui.cpp`: `sums[i + 1] = cumulative;`
            # `bookIndex.saveBookMetrics(..., sums, ...)`
            # So `sums` has `chapters + 1` entries.
            # `saveBookMetrics` writes `count = chapterOffsets.size()`.
            # So it writes `chapters + 1` entries.
            
            with open(metrics_path, "wb") as f:
                version = 1
                f.write(struct.pack("<B", version))
                f.write(struct.pack("<I", cumulative_length)) # TotalChars (size_t = 4 bytes on ESP32)
                
                count = len(chapter_offsets)
                f.write(struct.pack("<I", count))
                
                for offset in chapter_offsets:
                    f.write(struct.pack("<I", offset)) # size_t = 4 bytes
                    
            print(f"  -> Saved metrics: {cumulative_length} chars, {count} entries")
            return True
            
    except Exception as e:
        print(f"[ERROR] Failed to process {epub_path.name}: {e}")
        return False

def get_metadata(epub_path: Path) -> Tuple[Optional[str], Optional[str], int]:
    """
    Extract (title, author, file_size) from the EPUB.
    Returns (None, None, size) on failure.
    """
    if not epub_path.is_file():
        return None, None, 0

    size = epub_path.stat().st_size

    try:
        with zipfile.ZipFile(epub_path, "r") as zf:
            # 1. Read META-INF/container.xml
            try:
                container_data = zf.read("META-INF/container.xml")
            except KeyError:
                return None, None, size

            try:
                container_root = ET.fromstring(container_data)
            except Exception:
                return None, None, size

            ns_container = {
                "c": "urn:oasis:names:tc:opendocument:xmlns:container"
            }
            rootfile_elem = container_root.find(
                ".//c:rootfile", ns_container
            )
            if rootfile_elem is None:
                return None, None, size

            opf_path = rootfile_elem.get("full-path")
            if not opf_path:
                return None, None, size

            # 2. Read OPF file
            try:
                opf_data = zf.read(opf_path)
            except KeyError:
                return None, None, size

            try:
                opf_root = ET.fromstring(opf_data)
            except Exception:
                return None, None, size

            ns = {
                "dc": "http://purl.org/dc/elements/1.1/",
                "opf": "http://www.idpf.org/2007/opf",
            }

            title_el = opf_root.find(".//dc:title", ns)
            author_el = opf_root.find(".//dc:creator", ns)

            title = title_el.text.strip() if (title_el is not None and title_el.text) else ""
            author = author_el.text.strip() if (author_el is not None and author_el.text) else ""

            return title, author, size
    except Exception:
        return None, None, size


def rename_epub_by_metadata(epub_path_str: str) -> Optional[Path]:
    """
    Given the full path to a single .epub file, read its metadata (title & author)
    from the EPUB and rename the file to: 'Title - Author.epub'.

    Returns the new Path on success (or the original Path if no change was needed),
    or None if the file could not be renamed (e.g. missing metadata or conflict).
    """
    epub_path = Path(epub_path_str).expanduser().resolve()

    if not epub_path.is_file():
        print(f"[ERROR] Not a file: {epub_path}")
        return None

    if epub_path.suffix.lower() != ".epub":
        print(f"[ERROR] Not an .epub file: {epub_path.name}")
        return None

    def clean_filename(name: str) -> str:
        """Remove characters that are invalid in filenames and normalize whitespace."""
        if not name:
            return ""
        name = name.strip()
        invalid_chars = r'<>:"/\\|?*'
        name = re.sub(f"[{re.escape(invalid_chars)}]", " ", name)
        name = re.sub(r"\s+", " ", name).strip()
        return name

    def get_title_author(epub: Path) -> Tuple[Optional[str], Optional[str]]:
        """
        Extract (title, author) from the EPUB's OPF metadata.
        Returns (None, None) on failure.
        """
        try:
            with zipfile.ZipFile(epub, "r") as zf:
                # 1. Read META-INF/container.xml
                try:
                    container_data = zf.read("META-INF/container.xml")
                except KeyError:
                    print(f"[WARN] {epub.name}: META-INF/container.xml not found.")
                    return None, None

                try:
                    container_root = ET.fromstring(container_data)
                except Exception as e:
                    print(f"[WARN] {epub.name}: invalid container.xml: {e}")
                    return None, None

                ns_container = {
                    "c": "urn:oasis:names:tc:opendocument:xmlns:container"
                }
                rootfile_elem = container_root.find(
                    ".//c:rootfile", ns_container
                )
                if rootfile_elem is None:
                    print(f"[WARN] {epub.name}: <rootfile> not found in container.xml.")
                    return None, None

                opf_path = rootfile_elem.get("full-path")
                if not opf_path:
                    print(f"[WARN] {epub.name}: empty full-path in container.xml.")
                    return None, None

                # 2. Read OPF file
                try:
                    opf_data = zf.read(opf_path)
                except KeyError:
                    print(f"[WARN] {epub.name}: OPF '{opf_path}' not found in archive.")
                    return None, None

                try:
                    opf_root = ET.fromstring(opf_data)
                except Exception as e:
                    print(f"[WARN] {epub.name}: invalid OPF XML: {e}")
                    return None, None

                ns = {
                    "dc": "http://purl.org/dc/elements/1.1/",
                    "opf": "http://www.idpf.org/2007/opf",
                }

                title_el = opf_root.find(".//dc:title", ns)
                author_el = opf_root.find(".//dc:creator", ns)

                title = title_el.text.strip() if (title_el is not None and title_el.text) else ""
                author = author_el.text.strip() if (author_el is not None and author_el.text) else ""

                return title, author
        except Exception as e:
            print(f"[WARN] {epub.name}: error reading metadata: {e}")
            return None, None

    print(f"Processing: {epub_path.name}")

    title, author = get_title_author(epub_path)
    if title is None and author is None:
        print("  -> Skipping (no usable metadata).")
        return None

    title = clean_filename(title)
    author = clean_filename(author)

    # Fallback to original filename (without extension) if title missing
    if not title:
        title = clean_filename(epub_path.stem)

    if not title:
        print("  -> Skipping (no usable title).")
        return None

    if author:
        new_stem = f"{title} - {author}"
    else:
        new_stem = title

    new_stem = new_stem.strip()
    if not new_stem:
        print("  -> Skipping (empty target name).")
        return None

    new_name = new_stem + epub_path.suffix  # keep .epub
    target = epub_path.with_name(new_name)

    if target == epub_path:
        print("  -> Already has correct name.")
        return epub_path

    if target.exists():
        print(f"  -> Target '{target.name}' already exists, skipping.")
        return None

    try:
        epub_path.rename(target)
        print(f"  -> Renamed to: {target.name}")
        return target
    except Exception as e:
        print(f"[ERROR] Failed to rename '{epub_path.name}': {e}")
        return None



# Extensions to strip (Images and Fonts)
IMAGE_EXTS = {
    '.jpg', '.jpeg', '.png', '.gif', '.webp', '.bmp', '.tiff'
    # Note: SVG not included as it's vector, harder to downscale
}

FONT_EXTS = {
    '.ttf', '.otf', '.woff', '.woff2', '.eot'
}

REMOVE_EXTS = IMAGE_EXTS | FONT_EXTS



def get_image_format(ext):
    ext = ext.lower()
    if ext in ['.jpg', '.jpeg']:
        return 'JPEG'
    elif ext == '.png':
        return 'PNG'
    elif ext == '.gif':
        return 'GIF'
    elif ext == '.webp':
        return 'WEBP'
    elif ext == '.bmp':
        return 'BMP'
    elif ext == '.tiff':
        return 'TIFF'
    else:
        return None

def should_remove(filename):
    return os.path.splitext(filename)[1].lower() in REMOVE_EXTS

def process_epub(epub_path, mode='downscale'):
    print(f"Processing: {epub_path}")
    
    # No backup created
    
    temp_epub = epub_path + ".tmp"
    
    try:
        with zipfile.ZipFile(epub_path, 'r') as zin, zipfile.ZipFile(temp_epub, 'w', zipfile.ZIP_DEFLATED) as zout:
            
            # 1. Identify files to remove and downscale
            files_to_remove = set()
            images_to_downscale = set()
            all_files = zin.namelist()
            
            for f in all_files:
                ext = os.path.splitext(f)[1].lower()
                if ext in FONT_EXTS:
                    files_to_remove.add(f)
                elif ext in IMAGE_EXTS:
                    if mode == 'remove':
                        files_to_remove.add(f)
                    elif mode == 'downscale':
                        images_to_downscale.add(f)
            
            print(f"  Found {len(files_to_remove)} files to remove (fonts{' and images' if mode=='remove' else ''}).")
            if mode == 'downscale':
                print(f"  Found {len(images_to_downscale)} images to downscale.")
            
            # 2. Copy files, filtering OPF content
            for item in zin.infolist():
                if item.filename in files_to_remove:
                    continue
                
                content = zin.read(item.filename)
                
                # Downscale images if needed
                if item.filename in images_to_downscale:
                    try:
                        img = Image.open(BytesIO(content))
                        width, height = img.size
                        max_width, max_height = 960, 540
                        if width > max_width or height > max_height:
                            # Calculate new size maintaining aspect ratio
                            ratio = min(max_width / width, max_height / height)
                            new_width = int(width * ratio)
                            new_height = int(height * ratio)
                            img = img.resize((new_width, new_height), Image.Resampling.LANCZOS)
                            output = BytesIO()
                            ext = os.path.splitext(item.filename)[1]
                            format = get_image_format(ext)
                            if format:
                                img.save(output, format=format)
                                content = output.getvalue()
                                print(f"  Downscaled {item.filename} from {width}x{height} to {new_width}x{new_height}")
                            else:
                                print(f"  Warning: Unsupported image format for {item.filename}, keeping as is.")
                        else:
                            print(f"  Kept {item.filename} as is ({width}x{height})")
                    except Exception as e:
                        print(f"  Warning: Could not downscale {item.filename}: {e}, keeping as is.")
                
                # If it's an OPF file, remove references to deleted files
                if item.filename.endswith('.opf'):
                    try:
                        text = content.decode('utf-8')
                        new_lines = []
                        removed_count = 0
                        for line in text.splitlines():
                            # Check if this line references any removed file
                            drop_line = False
                            if '<item' in line:
                                for removed_f in files_to_remove:
                                    if os.path.basename(removed_f) in line:
                                        drop_line = True
                                        break
                            
                            if drop_line:
                                removed_count += 1
                            else:
                                new_lines.append(line)
                        
                        content = '\n'.join(new_lines).encode('utf-8')
                        print(f"  Removed {removed_count} manifest entries from {item.filename}")
                    except UnicodeDecodeError:
                        print(f"  Warning: Could not decode {item.filename}, copying as is.")
                
                if item.filename == 'mimetype':
                    zout.writestr(item, content, zipfile.ZIP_STORED)
                else:
                    zout.writestr(item, content)
                    
        # Success, replace original
        shutil.move(temp_epub, epub_path)
        print("  Done.")
        
    except Exception as e:
        print(f"  Error: {e}")
        if os.path.exists(temp_epub):
            os.remove(temp_epub)

def sanitize(s: str) -> str:
    """Sanitize string for index file, replacing problematic characters."""
    return s.replace('|', '-').replace('\n', ' ').replace('\r', ' ')

def process_single_epub(epub_path_str: str, mode: str):
    epub_path = Path(epub_path_str)
    if not epub_path.is_file():
        return None

    process_epub(str(epub_path), mode)
    final_path = rename_epub_by_metadata(str(epub_path))
    if final_path is None:
        final_path = epub_path

    has_metrics = 1 if generate_metrics(final_path) else 0

    title, author, size = get_metadata(final_path)
    if title is None:
        title = final_path.stem

    return {
        "name": final_path.name,
        "title": title,
        "author": author,
        "size": size,
        "has_metrics": has_metrics,
    }

def main():
    parser = argparse.ArgumentParser(description="Process EPUB files: downscale images or remove them.")
    parser.add_argument('target_dir', nargs='?', default=os.getcwd(), help="Directory containing EPUB files (default: current directory)")
    parser.add_argument('--mode', choices=['downscale', 'remove'], default='downscale', help="Mode: 'downscale' to resize large images, 'remove' to remove images (default: downscale)")
    parser.add_argument('--workers', type=int, default=0, help="Number of worker processes (0 = auto)")
    
    args = parser.parse_args()
    target_dir = args.target_dir
    mode = args.mode
        
    print(f"Scanning directory: {target_dir}")
    print(f"Mode: {mode}")
    
    epub_paths = [
        os.path.join(target_dir, filename)
        for filename in sorted(os.listdir(target_dir))
        if filename.lower().endswith('.epub')
    ]

    if not epub_paths:
        print("No .epub files found in this directory.")
        return

    max_workers = args.workers if args.workers and args.workers > 0 else (os.cpu_count() or 1)
    max_workers = max(1, min(max_workers, len(epub_paths)))

    results = []
    if max_workers > 1:
        with ProcessPoolExecutor(max_workers=max_workers) as executor:
            for result in executor.map(process_single_epub, epub_paths, itertools.repeat(mode)):
                if result:
                    results.append(result)
    else:
        for path in epub_paths:
            result = process_single_epub(path, mode)
            if result:
                results.append(result)

    index_entries = []
    book_id = 1
    for result in results:
        title = sanitize(result["title"])
        author = sanitize(result["author"]) if result["author"] else ""
        index_path = f"{SDCARD_BOOKS_ROOT}/{result['name']}"
        entry = f"{book_id}|{title}|0|0|{index_path}|{result['size']}|{result['has_metrics']}|{author}|0||1.0"
        index_entries.append(entry)
        book_id += 1
                
    # Write index.txt
    index_file = os.path.join(target_dir, "index.txt")
    with open(index_file, 'w', encoding='utf-8') as f:
        for entry in index_entries:
            f.write(entry + '\n')
    print(f"Generated index.txt with {len(index_entries)} entries.")

if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    main()
