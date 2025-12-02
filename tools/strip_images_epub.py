import os
import sys
import zipfile
import shutil
import os
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Optional, Tuple


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
REMOVE_EXTS = {
    # Images
    '.jpg', '.jpeg', '.png', '.gif', '.webp', '.svg', '.bmp', '.tiff',
    # Fonts
    '.ttf', '.otf', '.woff', '.woff2', '.eot'
}



def should_remove(filename):
    return os.path.splitext(filename)[1].lower() in REMOVE_EXTS

def process_epub(epub_path):
    print(f"Processing: {epub_path}")
    
    # No backup created
    
    temp_epub = epub_path + ".tmp"
    
    try:
        with zipfile.ZipFile(epub_path, 'r') as zin, zipfile.ZipFile(temp_epub, 'w', zipfile.ZIP_DEFLATED) as zout:
            
            # 1. Identify files to remove
            files_to_remove = set()
            all_files = zin.namelist()
            
            for f in all_files:
                if should_remove(f):
                    files_to_remove.add(f)
            
            print(f"  Found {len(files_to_remove)} files to remove (images/fonts).")
            
            # 2. Copy files, filtering OPF content
            for item in zin.infolist():
                if item.filename in files_to_remove:
                    continue
                
                content = zin.read(item.filename)
                
                # If it's an OPF file, we should try to remove references to the deleted files
                if item.filename.endswith('.opf'):
                    try:
                        text = content.decode('utf-8')
                        new_lines = []
                        removed_count = 0
                        for line in text.splitlines():
                            # Check if this line references any removed file
                            # Heuristic: check if the filename of a removed file appears in the line
                            # and the line looks like a manifest item
                            drop_line = False
                            if '<item' in line:
                                for removed_f in files_to_remove:
                                    # We match on basename to handle relative paths in href
                                    # e.g. removed_f = "OEBPS/images/img.jpg", href="images/img.jpg"
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

def main():
    target_dir = os.getcwd()
    if len(sys.argv) > 1:
        target_dir = sys.argv[1]
        
    print(f"Scanning directory: {target_dir}")
    
    count = 0
    # Only scan the top level directory, or recursively? 
    # "to all the epub files in the folder it runs in" implies top level of that folder.
    # But os.walk is safer if they have subfolders. Let's stick to os.listdir for "the folder it runs in" strictly, 
    # or os.walk for robustness. The user said "in the folder it runs in", usually implies flat scan.
    # I'll use os.listdir to be safe and simple.
    
    for filename in os.listdir(target_dir):
        if filename.lower().endswith('.epub'):
            full_path = os.path.join(target_dir, filename)
            if os.path.isfile(full_path):
                process_epub(full_path)
                rename_epub_by_metadata(full_path)
                count += 1
                
    if count == 0:
        print("No .epub files found in this directory.")

if __name__ == '__main__':
    main()