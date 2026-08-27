// png_to_cursor.cpp
//
// Command-line tool that converts a square PNG into an X11 cursor theme.
// Resizes the source image to standard cursor sizes using stb_image /
// stb_image_resize2 / stb_image_write, then compiles it with xcursorgen.
//
// Dependencies: xcursorgen, stb_image.h, stb_image_resize2.h, stb_image_write.h
// Usage: sudo ./png_to_cursor
//
// Note: needs root since it writes under /usr/share/icons/.
// Source PNG must be square (width == height), doesn't need to already
// be a standard size.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <filesystem>
#include <unistd.h>
#include <cstdlib>
#include <sys/types.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

// Standard X11 cursor sizes, in pixels.
const uint32_t STANDARD_SIZES[] = {16, 24, 32, 48, 64, 96, 128, 256};

// One resized cursor frame: its size, scaled hotspot, and file path.
// Keeping these together (instead of parallel vectors) avoids a
// size/file mismatch if a resize step gets skipped.
struct CursorFrame {
    int size;
    int xhot;
    int yhot;
    std::string path;
};

// Removes the temp files created while generating the cursor.
void cleanupTempFiles(const std::vector<std::string>& extraFiles = {}) {
    if (fs::exists("/tmp/temp_cursor.cfg")) fs::remove("/tmp/temp_cursor.cfg");
    if (fs::exists("/tmp/left_ptr")) fs::remove("/tmp/left_ptr");
    for (const auto& f : extraFiles) {
        if (fs::exists(f)) fs::remove(f);
    }
}

// Loads the source PNG, resizes it to targetSize x targetSize, and
// writes it out as an RGBA PNG.
bool resizeAndSavePNG(const std::string& srcPath, const std::string& dstPath, int targetSize) {
    int width, height, channels;

    // Force RGBA so it doesn't matter if the source is indexed/RGB/RGBA.
    unsigned char* imgData = stbi_load(srcPath.c_str(), &width, &height, &channels, 4);
    if (!imgData) {
        std::cerr << "Error: stbi_load failed -> " << stbi_failure_reason() << std::endl;
        return false;
    }

    std::vector<unsigned char> outputData(static_cast<size_t>(targetSize) * targetSize * 4);

    // Resize in linear color space, RGBA layout.
    unsigned char* resized = stbir_resize_uint8_linear(
        imgData, width, height, 0,
        outputData.data(), targetSize, targetSize, 0,
                                                       STBIR_RGBA
    );

    stbi_image_free(imgData);

    if (!resized) {
        std::cerr << "Error: stbir_resize_uint8_linear failed." << std::endl;
        return false;
    }

    int writeOk = stbi_write_png(
        dstPath.c_str(), targetSize, targetSize, 4,
                                 outputData.data(), targetSize * 4
    );

    if (!writeOk) {
        std::cerr << "Error: stbi_write_png failed -> " << dstPath << std::endl;
        return false;
    }

    return true;
}

// Scales a hotspot coordinate from the original image size to a target
// cursor size, keeping its relative position:
// hotspot_target = (hotspot_original * target) / original
int scaleHotspot(int originalHotspot, int originalSize, int targetSize) {
    if (originalSize <= 0) return 0;
    return static_cast<int>(
        (static_cast<double>(originalHotspot) * targetSize) / originalSize + 0.5
    );
}

int main() {

    // 1. ENVIRONMENT & PRIVILEGE CHECKS
    if (geteuid() != 0) {
        std::cerr << "This program requires root privileges. Please re-run using 'sudo'." << std::endl;
        return 1;
    }

    int result = std::system("xcursorgen --version > /dev/null 2>&1");
    if (result != 0) {
        std::cerr << "Error: 'xcursorgen' is not installed." << std::endl;
        std::cout << "For Ubuntu/Debian: sudo apt install x11-apps" << std::endl;
        std::cout << "For Fedora:        sudo dnf install xorg-x11-apps" << std::endl;
        std::cout << "For Arch Linux:    sudo pacman -S xcursor-themes" << std::endl;
        return 1;
    }

    std::string filePath;
    bool isSuccess = false;
    int srcWidth = 0, srcHeight = 0, srcChannels = 0;

    // 2. SOURCE FILE VALIDATION (via stb_image, no manual header parsing)
    while (!isSuccess) {
        std::cout << "\nPlease enter the absolute path to your PNG file: ";
        std::cin >> filePath;

        if (filePath == "exit" || filePath == "EXIT") {
            std::cout << "Exiting application. Goodbye!" << std::endl;
            cleanupTempFiles();
            return 0;
        }

        if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
            std::cerr << "Error: File not found or invalid path. Please try again." << std::endl;
            continue;
        }

        // stbi_info only reads the header, so this is a cheap check.
        if (!stbi_info(filePath.c_str(), &srcWidth, &srcHeight, &srcChannels)) {
            std::cerr << "Error: Not a valid image file (" << stbi_failure_reason() << ")." << std::endl;
            continue;
        }

        if (srcWidth != srcHeight) {
            std::cerr << "Error: Image must be square (width must equal height). Got: "
            << srcWidth << "x" << srcHeight << std::endl;
            continue;
        }

        std::cout << "Success: Valid " << srcWidth << "x" << srcHeight << " square image found." << std::endl;
        isSuccess = true;
    }

    // 3. TARGET SIZE SELECTION (source doesn't need to already match a
    // standard size, since it gets resized)
    std::cout << "\nStandard cursor sizes: 16 24 32 48 64 96 128 256" << std::endl;
    std::cout << "Enter a single size (e.g. 32), or 'all' for a multi-resolution cursor: ";
    std::string sizeInput;
    std::cin >> sizeInput;

    std::vector<int> targetSizes;
    if (sizeInput == "all" || sizeInput == "ALL") {
        for (uint32_t s : STANDARD_SIZES) targetSizes.push_back(static_cast<int>(s));
    } else {
        int chosen = std::atoi(sizeInput.c_str());
        bool valid = false;
        for (uint32_t s : STANDARD_SIZES) {
            if (static_cast<int>(s) == chosen) { valid = true; break; }
        }
        if (!valid) {
            std::cerr << "Error: Invalid size. Please choose one from the standard list." << std::endl;
            return 1;
        }
        targetSizes.push_back(chosen);
    }

    // 3b. HOTSPOT SELECTION (relative to the original image)
    //
    // The hotspot is the exact "click point" pixel (e.g. the tip of an
    // arrow cursor). Most users don't know the exact coordinates, so we
    // offer presets: top-left (default for arrow-style cursors), center
    // (for crosshair/symmetric icons), or a custom coordinate.
    std::cout << "\nThe hotspot is the exact 'click point' of the cursor." << std::endl;
    std::cout << "1) Top-left (0,0)   - recommended for arrow/pointer style cursors" << std::endl;
    std::cout << "2) Center           - recommended for crosshair/symmetric icons" << std::endl;
    std::cout << "3) Custom           - enter exact coordinates" << std::endl;
    std::cout << "Choose [1-3] (default 1): ";

    std::string hotspotChoice;
    std::cin >> hotspotChoice;

    int originalXhot = 0;
    int originalYhot = 0;

    if (hotspotChoice == "2") {
        originalXhot = srcWidth / 2;
        originalYhot = srcHeight / 2;
    } else if (hotspotChoice == "3") {
        std::cout << "Enter hotspot X (0-" << (srcWidth - 1) << "): ";
        std::cin >> originalXhot;
        std::cout << "Enter hotspot Y (0-" << (srcHeight - 1) << "): ";
        std::cin >> originalYhot;
    }
    // Any other input (including "1") falls through to the top-left default.

    // Clamp hotspot to valid image bounds.
    originalXhot = std::max(0, std::min(originalXhot, srcWidth - 1));
    originalYhot = std::max(0, std::min(originalYhot, srcHeight - 1));

    // 4. THEME NAME & TARGET DIRECTORY
    std::string icontheme;
    std::string targetDir;
    bool validThemeName = false;

    while (!validThemeName) {
        std::cout << "\nEnter your icon theme name (!English characters only!): ";
        std::cin >> icontheme;

        if (icontheme == "exit" || icontheme == "EXIT") {
            std::cout << "Exiting application. Goodbye!" << std::endl;
            cleanupTempFiles();
            return 0;
        }

        targetDir = "/usr/share/icons/" + icontheme + "/cursors";

        if (fs::exists("/usr/share/icons/" + icontheme)) {
            std::cerr << "Error: A theme with the name '" << icontheme
            << "' already exists. Please choose a different name." << std::endl;
        } else {
            validThemeName = true;
        }
    }

    fs::create_directories(targetDir);

    std::ofstream themeFile("/usr/share/icons/" + icontheme + "/index.theme");
    if (themeFile.is_open()) {
        themeFile << "[Icon Theme]\nName=" << icontheme << "\n";
        themeFile << "Inherits=Breeze,breeze_cursors,Adwaita,Yaru,core\n";
        themeFile.close();
    }

    // 5. RESIZE + SAVE PNG FOR EACH TARGET SIZE
    //
    // Each successful frame is stored as a CursorFrame instead of
    // parallel vectors, so a size and its file can't get out of sync if
    // a resize step fails and gets skipped.
    std::vector<CursorFrame> frames;
    for (int size : targetSizes) {
        std::string resizedPath = "/tmp/temp_cursor_" + std::to_string(size) + ".png";
        std::cout << "Resizing to " << size << "x" << size << " ..." << std::endl;

        if (!resizeAndSavePNG(filePath, resizedPath, size)) {
            std::cerr << "Error: Resize failed for size " << size << ", skipping." << std::endl;
            continue;
        }

        CursorFrame frame;
        frame.size = size;
        frame.xhot = scaleHotspot(originalXhot, srcWidth, size);
        frame.yhot = scaleHotspot(originalYhot, srcHeight, size);
        frame.path = resizedPath;
        frames.push_back(frame);
    }

    if (frames.empty()) {
        std::cerr << "Error: No sizes were resized successfully. Aborting." << std::endl;
        cleanupTempFiles();
        return 1;
    }

    // 6. GENERATE THE .cfg FILE (one line per size enables multi-res cursors)
    // Format: <size> <xhot> <yhot> <file_path>
    std::ofstream cfgFile("/tmp/temp_cursor.cfg");
    if (cfgFile.is_open()) {
        for (const auto& frame : frames) {
            cfgFile << frame.size << " " << frame.xhot << " " << frame.yhot
            << " " << frame.path << "\n";
        }
        cfgFile.close();
    }

    // 7. COMPILE WITH xcursorgen
    std::string command = "xcursorgen /tmp/temp_cursor.cfg /tmp/left_ptr";
    int resultcmd = std::system(command.c_str());

    // 8. DEPLOY TO THE SYSTEM ICON DIRECTORY
    if (resultcmd == 0) {
        std::cout << "Success: Cursor file successfully created!" << std::endl;

        std::string finalPath = targetDir + "/left_ptr";
        fs::copy_file("/tmp/left_ptr", finalPath, fs::copy_options::overwrite_existing);

        std::string defaultSymlink = targetDir + "/default";
        if (fs::exists(defaultSymlink) || fs::is_symlink(defaultSymlink)) {
            fs::remove(defaultSymlink);
        }

        std::error_code ec;
        fs::create_symlink("left_ptr", defaultSymlink, ec);
        if (!ec) {
            std::cout << "Success: Created 'default' symlink -> left_ptr" << std::endl;
        } else {
            std::cerr << "Warning: Failed to create 'default' symlink: " << ec.message() << std::endl;
        }

    } else {
        std::cerr << "Error: Failed to execute xcursorgen or an internal error occurred!" << std::endl;
    }

    // Collect resized file paths for cleanup.
    std::vector<std::string> resizedPaths;
    resizedPaths.reserve(frames.size());
    for (const auto& frame : frames) resizedPaths.push_back(frame.path);

    cleanupTempFiles(resizedPaths);

    return 0;
}
