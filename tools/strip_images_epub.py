import os
import sys
import zipfile
import shutil

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
                count += 1
                
    if count == 0:
        print("No .epub files found in this directory.")

if __name__ == '__main__':
    main()