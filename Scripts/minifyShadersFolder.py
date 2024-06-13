# Minifies all the HLSL or HLSLI files in the provided folder. Removing unneeded characters (including comments and extra whitespace) 
# then pack into a set of strings in a C++ header, along with a directory map to access by filename.

import glob
import os
import shutil
import sys
import re
import subprocess
import math
import platform

# Dump usage and exit if we have incorrect number of args.
if(len(sys.argv) < 3 or len(sys.argv) > 6):
    print("Usage: python minifyShadersFolder.py inputFolder outputFile [slangCompiler] [mainEntryPoint] [slangTarget]")
    print("  inputFolder - Folder containing Slang files")
    print("  outputMinifiedFile - Output minified C++ header file")
    print("  slangCompiler - Folder for slang executable to compile main entryPoint file (if omitted, just minifies no compilation)")
    print("  mainEntryPoint - Main entry point filename, also used for precompiled DXIL C++ header filename (if omitted, assumes MainEntryPoint)")
    print("  slangTarget - Slangc compiling target (metal, dxil, spirv)")
    sys.exit(-1)

# Parse the command line arguments.
hlslFolder = sys.argv[1]
outputFile = sys.argv[2]
nameSpaceName = os.path.splitext(os.path.basename(outputFile))[0] # Default namespace is output filename without extension or path.
slangc = ""
if(len(sys.argv) == 4):
    slangc = sys.argv[3]
mainEntryPoint = "MainEntryPoints"
if(len(sys.argv) == 5):
    mainEntryPoint = sys.argv[4]
target = ""
if(len(sys.argv) == 6):
    mainEntryPoint = sys.argv[5]

# Build list of files
files = glob.glob(hlslFolder + '/*.*sl*')

# See if any of the HLSL files are newer than output header file.
try:
    outfileStat = os.stat(outputFile)
    outputModTime = outfileStat.st_mtime
except:
    outputModTime = 0

# Compare the modified date on all the HLSL files in folder to output header file.
numFiles = len(files)
n = 0
rebuildNeeded = False
entryPointFile = ""
for hlslFile in files:
    if(mainEntryPoint in hlslFile):
        entryPointFile = hlslFile
    fileModTime = os.stat(hlslFile).st_mtime
    if(fileModTime > outputModTime):
        rebuildNeeded = True

# Early out, if nothing to be done.
if(not rebuildNeeded):
    print("Nothing to be done, output header file %s up to date" % (outputFile))
    sys.exit(0)
    
# If we have a slang compiler and entry point filename, then precompile it.
if(len(slangc) > 0 and len(entryPointFile) > 0):
    # Use platform default target if not provided.
    if (target != "metal" or target != "dxil" or target != "spirv"):
        if(platform.system() == "Darwin"):
            target = "metal"
        elif(platform.system() == "Windows"):
            target = "dxil"
        elif(platform.system() == "Linux"):
            target = "spirv"
        else:
            print("Unsupported platform: " + platform.system())
            sys.exit(-1)

    compiledFile = ""
    compiledTempFile = ""
    variableName = ""
    cmd = []
    if (target == "metal"):
        headerFileName = mainEntryPoint + ".h"
        metalFileName = mainEntryPoint + ".metal"
        compiledFile = os.path.join(os.path.dirname(outputFile), headerFileName)
        print("Compiling main entry point %s with Slang compiler %s to MSL in header %s"%(entryPointFile, slangc, compiledFile))
        compiledTempFile = os.path.join(os.path.dirname(outputFile), metalFileName)
        variableName = "g_s" + mainEntryPoint + "metal"
        cmd = [slangc, entryPointFile, "-target", target, "-o", compiledTempFile]
        # cmd = [slangc, entryPointFile, "-DRUNTIME_COMPILE_EVALUATE_MATERIAL_FUNCTION=1", "-target", "metal", "-profile", "lib_6_3", "-o", compiledTempFile]'
    elif (target == "dxil"):
        compiledFile = os.path.dirname(outputFile) + "/" + mainEntryPoint + ".h"
        print("Compiling main entry point %s with Slang compiler %s to DXIL in header %s"%(entryPointFile, slangc, compiledFile))
        compiledTempFile = os.path.dirname(outputFile) +"/"+ mainEntryPoint + ".dxil"
        variableName = "g_s" + mainEntryPoint + "DXIL"
        cmd = [slangc, entryPointFile, "-DDIRECTX=1", "-DRUNTIME_COMPILE_EVALUATE_MATERIAL_FUNCTION=1", "-target", target, "-profile", "lib_6_3", "-o", compiledTempFile]
    else: # "spirv"
        compiledFile = os.path.dirname(outputFile) + "/" + mainEntryPoint + ".h"
        print("Compiling main entry point %s with Slang compiler %s to SPIRV in header %s"%(entryPointFile, slangc, compiledFile))
        compiledTempFile = os.path.dirname(outputFile) +"/"+ mainEntryPoint + ".spv"
        variableName = "g_s" + mainEntryPoint + "SPIRV"
        cmd = [slangc, entryPointFile, "-target", "spirv", "-o", compiledTempFile]

    result = subprocess.run(cmd, capture_output=True)
    if(result.returncode != 0):
        print("Compliation failed on " + entryPointFile)
        print(result.stderr.decode())
        sys.exit(-1)
    f = open(compiledTempFile, mode = "rb")
    data = bytearray(f.read())
    numBytes = len(data)
    numWords = math.ceil(numBytes / 4)
    headerStr = "static const array<unsigned int, " + str(numWords) + "> " + variableName + " = {\n"
    for i in range(numWords):
        wordBytes = bytes([data[i * 4 + 3],data[i * 4 + 2],data[i * 4 + 1],data[i * 4 + 0]])
        headerStr += "0x" + str(wordBytes.hex())
        if(i < numWords - 1):
            headerStr += ", "
        if(i%8==7):
            headerStr += "\n"
    headerStr += "};\n"
    with open(compiledFile, 'w') as f:
        f.write(headerStr)    


print("Minifying %d files from %s" % (numFiles, hlslFolder))

# Open header output file (exit if open fails.)
try:
    headerOutput = open(outputFile , "w")
except:
    print("Failed to open output header: " + outputFile)
    sys.exit(-1)
    
# Begin directory map string
directorySymbolName = "g_sDirectory"
directoryString = '\n\t// Directory map, used to get file contents from HLSL filename.\n'
directoryString = directoryString + '\tstatic const std::map<std::string , const std::string &> ' + directorySymbolName + ' = {\n'

# Begin header file with namespace
headerOutput.write('// Minified HLSL header file.\n// Automatically generated by ' + sys.argv[0] + ' from ' + str(numFiles) + ' files in folder ' + hlslFolder + '\n')
headerOutput.write('namespace ' + nameSpaceName + ' {\n')

# Minify all the HLSL files
for hlslFile in files:
    # Print message
    print("\t Minifying file %d of %d: %s" % ((n + 1), numFiles, hlslFile))

    # Get base filename (with path)
    baseFilename = os.path.basename(hlslFile)
    # Open pre-HLSL file (exit if open fails.)
    try:
        hlslFileStream = open(hlslFile, "r")
    except:
        print("Failed to open: " + hlslFile)
        sys.exit(-1)

    # Read all lines of HLSL to an array.
    lines = hlslFileStream.readlines()
        
    # Work out string symbol name from path (based filename without extension)
    stringName = "g_s" + os.path.splitext(os.path.basename(hlslFile))[0]

    # Check we don't have a name collision with the directory map object.
    if(stringName == directorySymbolName):
        print("Invalid HLSL filename " + baseFilename + " name conflict with built-in directory map named " + directorySymbolName)

    # Begin minified  C string.
    headerOutput.write('\n\t// Minified string created from ' + hlslFile + '\n')
    headerOutput.write('\tstatic const std::string ' + stringName + ' = \n')

    # Prepend a #line statement to help with debugging
    lines.insert(0,  '#line 1 \"' + re.sub(r"\\", "/", hlslFile) + '\"\n')

    # Current header file output line and line number.
    currOutputStr = ""
    lineNo = 0

    # Iterate through all the lines.
    for ln in lines:
        # Remove single line comments (TODO: Remove multi-line comments)
        ln = re.sub(r"//.*\n", "\n", ln)
        # Replace single backslash with double backslash in C source (8x required as python AND DXC needed escaping).
        ln = re.sub(r"\\", "\\\\\\\\", ln)
        # Escape newlines
        ln = re.sub(r"\n", "\\\\n", ln)
        # Remove extra non-newline whitespace.
        ln = re.sub(r"[^\S\r\n]+", " ", ln)
        # Escape quotes.
        ln = re.sub(r"\"", "\\\"", ln)
    
        # Increment line number.
        lineNo = lineNo + 1

        # Append newline to end of file.
        if(lineNo == len(lines)):
            ln += "\\n"

        # Append to current line in header file string.
        currOutputStr += ln

        # When current line longer than 100 chars, write to header string (wrapping in quotes and adding newline + backslash)
        if(len(currOutputStr) > 100):
            headerOutput.write('\t\"' + currOutputStr + '\" \\\n')
            currOutputStr = ""

    # Finish writing minified string.
    headerOutput.write('\t\t\"' + currOutputStr + '\" \n\t;\n')

    # Add entry to directory map string.
    n = n + 1
    directoryString = directoryString + ('\t\t{ "%s", %s }'% (baseFilename, stringName))
    if(n == len(files)):
        directoryString = directoryString + "\n\t};\n"
    else:
        directoryString = directoryString + ",\n"

# Write directory map string
headerOutput.write(directoryString)
# End namespace
headerOutput.write("}\n")
# Close header file.
headerOutput.close()
