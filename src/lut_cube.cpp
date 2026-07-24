#include "lut_cube.hpp"

#include "logger.hpp"

namespace VKIntox
{
    LutCube::LutCube()
    {
    }
    LutCube::LutCube(const std::string& file)
    {
        std::ifstream cubeStream(file);
        if (!cubeStream.good())
        {
            Logger::err("lut cube file does not exist: " + file);
            return;
        }

        std::string line;

        while (std::getline(cubeStream, line))
        {
            parseLine(line);
        }
    }
    void LutCube::parseLine(std::string line)
    {
        if (line.length() == 0)
        {
            return;
        }
        if (line[0] == '#')
        {
            return;
        }
        if (line.find("LUT_3D_SIZE") != std::string::npos)
        {
            line = line.substr(line.find("LUT_3D_SIZE") + 11);
            line = skipWhiteSpace(line);
            size = std::stoi(line);

            colorCube = std::vector<unsigned char>(size * size * size * 4, 255);
            return;
        }
        if (line.find("DOMAIN_MIN") != std::string::npos)
        {
            line = line.substr(line.find("DOMAIN_MIN") + 10);
            splitTripel(line, minX, minY, minZ);
            return;
        }
        if (line.find("DOMAIN_MAX") != std::string::npos)
        {
            line = line.substr(line.find("DOMAIN_MAX") + 10);
            splitTripel(line, maxX, maxY, maxZ);
            return;
        }
        if (line.find_first_of("0123456789") == 0)
        {
            if (size == 0 || colorCube.empty())
                return;

            float         x, y, z;
            unsigned char outX, outY, outZ;
            splitTripel(line, x, y, z);
            clampTripel(x, y, z, outX, outY, outZ);
            writeColor(currentX, currentY, currentZ, outX, outY, outZ);
            if (currentX != size - 1)
            {
                currentX++;
            }
            else if (currentY != size - 1)
            {
                currentY++;
                currentX = 0;
            }
            else if (currentZ != size - 1)
            {
                currentZ++;
                currentX = 0;
                currentY = 0;
            }
            return;
        }
    }

    std::string LutCube::skipWhiteSpace(std::string text)
    {
        while (text.size() > 0 && (text[0] == ' ' || text[0] == '\t'))
        {
            text = text.substr(1);
        }
        return text;
    }

    void LutCube::splitTripel(std::string tripel, float& x, float& y, float& z)
    {
        tripel       = skipWhiteSpace(tripel);
        size_t after = tripel.find_first_of(" \n");
        if (after == std::string::npos)
        {
            x = std::stof(tripel);
            y = 0.0f;
            z = 0.0f;
            return;
        }
        x            = std::stof(tripel.substr(0, after));
        tripel       = tripel.substr(after);

        tripel = skipWhiteSpace(tripel);
        after  = tripel.find_first_of(" \n");
        if (after == std::string::npos)
        {
            y = std::stof(tripel);
            z = 0.0f;
            return;
        }
        y      = std::stof(tripel.substr(0, after));
        tripel = tripel.substr(after);

        tripel = skipWhiteSpace(tripel);
        z      = std::stof(tripel);
    }

    void LutCube::clampTripel(float x, float y, float z, unsigned char& outX, unsigned char& outY, unsigned char& outZ)
    {
        float rangeX = maxX - minX;
        float rangeY = maxY - minY;
        float rangeZ = maxZ - minZ;

        // Guard against division by zero for degenerate domains
        if (rangeX == 0.0f) rangeX = 1.0f;
        if (rangeY == 0.0f) rangeY = 1.0f;
        if (rangeZ == 0.0f) rangeZ = 1.0f;

        outX = static_cast<unsigned char>(255.0f * (x / rangeX));
        outY = static_cast<unsigned char>(255.0f * (y / rangeY));
        outZ = static_cast<unsigned char>(255.0f * (z / rangeZ));
    }

    void LutCube::writeColor(int x, int y, int z, unsigned char r, unsigned char g, unsigned char b)
    {
        if (size == 0)
            return;

        // Bounds check to prevent buffer overflow from malformed .cube files
        if (x < 0 || x >= size || y < 0 || y >= size || z < 0 || z >= size)
            return;

        static const int colorSize = 4; // 4 bytes per point in the cube, rgba

        int locationR = (((z * size) + y) * size + x) * colorSize;

        colorCube[locationR + 0] = r;
        colorCube[locationR + 1] = g;
        colorCube[locationR + 2] = b;
    }
} // namespace VKIntox