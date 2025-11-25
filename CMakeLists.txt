cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5paper_ebook_reader)

spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)
