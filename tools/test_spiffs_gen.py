import sys
import os
import shutil

# Add ESP-IDF spiffs component to path
idf_path = os.environ.get('IDF_PATH')
if not idf_path:
    # Try to guess from previous output
    idf_path = "C:/Espressif/frameworks/esp-idf-v5.5"

spiffs_path = os.path.join(idf_path, 'components', 'spiffs')
sys.path.append(spiffs_path)

try:
    import spiffsgen
except ImportError:
    print(f"Could not import spiffsgen from {spiffs_path}")
    sys.exit(1)

def test_spiffs_generation():
    base_dir = "test_spiffs_dir"
    if os.path.exists(base_dir):
        shutil.rmtree(base_dir)
    os.makedirs(base_dir)
    
    # Create a file with Hebrew name
    filename = "ספר המסעות אפריים קישון.epub"
    with open(os.path.join(base_dir, filename), 'w', encoding='utf-8') as f:
        f.write("Hello World" * 100)
        
    # Create a file in a subdirectory
    os.makedirs(os.path.join(base_dir, "fonts"), exist_ok=True)
    with open(os.path.join(base_dir, "fonts", "NotoSansHebrew-Regular.vlw"), 'w', encoding='utf-8') as f:
        f.write("Font Data")
        
    image_file = "test_spiffs.bin"
    
    # Args: image_size, base_dir, output_file, page_size, obj_name_len, meta_len, use_magic, use_magic_len
    # spiffsgen.main() parses args. We can call spiffsgen.SpiffsBuildConfig directly?
    # Or just invoke main with sys.argv
    
    sys.argv = [
        "spiffsgen.py",
        "0x100000", # 1MB
        base_dir,
        image_file,
        "--page-size=256",
        "--obj-name-len=64",
        "--meta-len=4",
        "--use-magic",
        "--use-magic-len"
    ]
    
    print("Running spiffsgen with Hebrew filename...")
    try:
        spiffsgen.main()
        print("Success!")
    except Exception as e:
        print(f"Failed: {e}")
        import traceback
        traceback.print_exc()
        
    # Clean up
    # shutil.rmtree(base_dir)
    # if os.path.exists(image_file):
    #    os.remove(image_file)

if __name__ == "__main__":
    test_spiffs_generation()