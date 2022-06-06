# -*- coding: utf-8 -*-
"""
Created on Mon May 30 11:06:12 2022

@author: user
"""

FILENAME = "example.txt"                               #Name of the file with Blockly code

def addQuotationMarks(word):
    return "\"" + word + "\""  


def getNamespace(fileLines):                           #Used to find the namespace
    
    lineOfInterest = fileLines[0]
    
    startIdx = lineOfInterest.find("__") + 2
    endIdx = lineOfInterest.find("_", startIdx)
    
    return (lineOfInterest[startIdx : endIdx])


def getBlockNames(fileLines, namespace):               #Used to create a list of block names
    
    blockNames = []
    
    for line in fileLines:
        if (line.find("// Block") != -1):
            
            startIdx = line.find("__" + namespace) + len(namespace) + 3
            blockNames.append(line[startIdx : -1])
    
    return blockNames


def getColor(fileLines):                               #Used to get a color of the blocks
    
    for line in fileLines:
        if (line.find("colour") != -1):
            
            startIdx = line.find("colour") + 10
            return (line[startIdx : -2])


def insertBackSlashes(fileLines):                      #Used to enter backslashes before ' " ' signs

    newFileLines = [line.replace("\"", ("\\\"")) for line in fileLines]
    return newFileLines
        

def getBlockCodePart(fileLines, blockName, namespace): #Used to extract code
    
    blockCodePartOne = ""
    blockCodePartTwo = ""
    
    identifier = "__" + namespace + "_" + blockName
    desiredLine = "window[\'Blockly\'].Python[\'" + identifier
    startIdx = None
    endIdx = None
    
    for idx, line in enumerate(fileLines):
        if (line.find(desiredLine) != -1):
            startIdx = idx
            break
            
    for idx, line in enumerate(fileLines[idx : ]):
        if (line.find("};") != -1):
            endIdx = idx + startIdx
            break
    
    
    for index in range (startIdx, endIdx + 1):
        blockCodePartOne += fileLines[index]
    
    
    
    for idx, line in enumerate(fileLines[startIdx : endIdx]):
        if (line.find("return") != -1):
            startIdx = startIdx + idx
            startLineIdx = line.find("`") + 1
            break
    
    for index in range(startIdx, endIdx):
        if ((fileLines[index].find(' + \"\\n\"') != -1) or (fileLines[index].find(", Blockly.Python.ORDER_CONDITIONAL") != -1)):
            
            newEndIdx = index
            
            if (fileLines[index].find(' + \"\\n\"') != -1):
                endLineIdx = fileLines[index].find(' + \"\\n\"') - 1
            else:
                endLineIdx = fileLines[index].find(", Blockly.Python.ORDER_CONDITIONAL") - 1
            break

    if (startIdx == newEndIdx):
        blockCodePartTwo += fileLines[startIdx][startLineIdx : endLineIdx]
    
    else:
        blockCodePartTwo += fileLines[startIdx][startLineIdx : ]
        for index in range (startIdx + 1, newEndIdx):
            blockCodePartTwo += fileLines[index]
        blockCodePartTwo += fileLines[newEndIdx][ : endLineIdx]
    
    return (blockCodePartOne, blockCodePartTwo)
    
         

file = open(FILENAME, "r")
fileLines = file.readlines()
file.close()

fileLinesBackSlashes = insertBackSlashes(fileLines)    #Stores the code lines with backlashes before the ' "" ' signs

namespace = getNamespace(fileLines)
blocks = getBlockNames(fileLines, namespace)
color = getColor(fileLines)

code = "{" + addQuotationMarks("category") + ":" + addQuotationMarks(namespace) + ","
code += addQuotationMarks("color") + ":" + addQuotationMarks(color) + ","
code += addQuotationMarks("blocks") + ":["

for idx, block in enumerate(blocks):
    code += addQuotationMarks("__" + namespace + "_" + block)
    if (idx == (len(blocks) - 1)):
        break
    code += ","

code += "],"
code += addQuotationMarks("jscode") + ":" + "\""

for line in insertBackSlashes(fileLines):
    code += line

code += "\n\",\"code\":{"

for idx, block in enumerate(blocks):
    code += addQuotationMarks(block)
    code += ":[\""
    code += insertBackSlashes([getBlockCodePart(fileLines, block, namespace)[0]])[0]
    code += "\n\",\""
    code += insertBackSlashes([getBlockCodePart(fileLines, block, namespace)[1]])[0]
    code += "\"]"
    if (idx == (len(blocks) - 1)):
        break
    code += ","

code +="}}"

code = code.replace("\n", "\\n")
code = code.replace("\\\"\\n\\\"", "\\\"\\\\n\\\"")

saveFile = open("myfile.m5b", "w")
saveFile.write(code)
saveFile.close()