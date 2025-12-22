import os
from PIL import Image

# Directory containing the icons
script_dir = os.path.dirname(os.path.abspath(__file__))
icon_dir = os.path.join(script_dir, '../spiffs_image/icons')
icon_dir = os.path.abspath(icon_dir)

# Ensure the directory exists
if not os.path.exists(icon_dir):
    print(f"Directory {icon_dir} does not exist.")
    exit(1)

# Process each file in the directory
for filename in os.listdir(icon_dir):
    if filename.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp', '.gif')):
        filepath = os.path.join(icon_dir, filename)
        try:
            img = Image.open(filepath)
            img_resized = img.resize((96, 96), Image.Resampling.LANCZOS)
            img_resized.save(filepath)
            print(f"Resized {filename} to 96x96")
        except Exception as e:
            print(f"Error processing {filename}: {e}")

print("All images resized.")