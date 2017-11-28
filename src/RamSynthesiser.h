/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RamSynthesiser.h
 *
 * Declares entities capable of executing a RAM program.
 *
 ***********************************************************************/

#pragma once

#include "RamExecutor.h"

#include <string>

namespace souffle {

/**
 * A RAM executor based on the creation and compilation of an executable conducting
 * the actual computation.
 */
class RamCompiler : public RamExecutor {
private:
    std::string compileCmd;

public:
    /** A simple constructor */
    RamCompiler(const std::string& compileCmd) : compileCmd(compileCmd) {}

    /**
     * Generates the code for the given ram statement.The target file
     * name is either set by the corresponding member field or will
     * be determined randomly. The chosen file-name will be returned.
     */
    std::string generateCode(const SymbolTable& symTable, const RamProgram& prog,
            const std::string& filename = "", const int index = -1) const;

    /**
     * Generates the code for the given ram statement.The target file
     * name is either set by the corresponding member field or will
     * be determined randomly. The chosen file-name will be returned.
     */
    std::string compileToLibrary(const SymbolTable& symTable, const RamProgram& prog,
            const std::string& filename = "default", const int index = -1) const;

    /**
     * Compiles the given statement to a binary file. The target file
     * name is either set by the corresponding member field or will
     * be determined randomly. The chosen file-name will be returned.
     * Note that this uses the generateCode method for code generation.
     */
    std::string compileToBinary(const SymbolTable& symTable, const RamProgram& prog,
            const std::string& filename = "", const int index = -1) const;

    /**
     * Compiles the given statement to a binary file. The target file
     * name is either set by the corresponding member field or will
     * be determined randomly. The environment after execution will be returned.
     * Note that this uses the compileToBinary method for code compilation.
     */
    std::string executeBinary(const SymbolTable& symTable, const RamProgram& prog,
            const std::string& filename = "", const int index = -1) const;

    /**
     * The actual implementation of this executor encoding the given
     * program into a source file, compiling and executing it.
     */
    void applyOn(const RamProgram& prog, RamEnvironment& env, RamData* data) const override;

    /**
     * Execute sub-routines  
     */
    virtual void executeSubroutine(RamEnvironment& env, const RamStatement& stmt,
            const std::vector<RamDomain>& arguments, std::vector<RamDomain>& returnValues,
            std::vector<bool>& returnErrors) const override {
        // nothing to do here
    }
};


}  // end of namespace souffle
