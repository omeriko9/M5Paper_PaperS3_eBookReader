import os
import sys
import zipfile
import shutil
import re
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Optional, Tuple
from PIL import Image
from io import BytesIO
import argparse


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

def main():
    parser = argparse.ArgumentParser(description="Process EPUB files: downscale images or remove them.")
    parser.add_argument('target_dir', nargs='?', default=os.getcwd(), help="Directory containing EPUB files (default: current directory)")
    parser.add_argument('--mode', choices=['downscale', 'remove'], default='downscale', help="Mode: 'downscale' to resize large images, 'remove' to remove images (default: downscale)")
    
    args = parser.parse_args()
    target_dir = args.target_dir
    mode = args.mode
        
    print(f"Scanning directory: {target_dir}")
    print(f"Mode: {mode}")
    
    count = 0
    
    for filename in os.listdir(target_dir):
        if filename.lower().endswith('.epub'):
            full_path = os.path.join(target_dir, filename)
            if os.path.isfile(full_path):
                process_epub(full_path, mode)
                rename_epub_by_metadata(full_path)
                count += 1
                
    if count == 0:
        print("No .epub files found in this directory.")

if __name__ == '__main__':
    main()