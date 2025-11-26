import os
import sys

def rename_epubs(directory):
    print(f"Renaming EPUBs in {directory}...")
    
    files = [f for f in os.listdir(directory) if f.lower().endswith('.epub')]
    files.sort() # Sort to have deterministic order
    
    count = 0
    for i, filename in enumerate(files):
        old_path = os.path.join(directory, filename)
        
        # Check if already renamed to avoid double renaming if run multiple times
        # Pattern: book_X.epub
        if filename.startswith("book_") and filename[5:-5].isdigit():
            print(f"Skipping {filename}, already renamed.")
            continue
            
        new_name = f"book_{i+1}.epub"
        new_path = os.path.join(directory, new_name)
        
        # Handle collision if target exists (e.g. from previous run)
        while os.path.exists(new_path):
            i += 1
            new_name = f"book_{i+1}.epub"
            new_path = os.path.join(directory, new_name)
            
        try:
            os.rename(old_path, new_path)
            print(f"Renamed: '{filename}' -> '{new_name}'")
            count += 1
        except Exception as e:
            print(f"Error renaming {filename}: {e}")
            
    print(f"Renamed {count} files.")

if __name__ == "__main__":
    target_dir = "spiffs_image"
    if len(sys.argv) > 1:
        target_dir = sys.argv[1]
        
    if os.path.exists(target_dir):
        rename_epubs(target_dir)
    else:
        print(f"Directory {target_dir} not found.")