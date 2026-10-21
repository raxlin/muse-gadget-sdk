/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/*
 * The pet's pixel art, in the same string form the character is drawn with:
 * '#' is the main colour, 'o' the accent, '.' is left clear.
 *
 * One definition serves both uses, so a button and the items the character
 * throws up when you press it can never drift apart: muse_pixel.c stamps these
 * into the 64x64 character grid, and muse_ui.c blows the same rows up into the
 * button icons.
 */

#define MUSE_PET_ART_W 5
#define MUSE_PET_ART_H 5

static const char *const MUSE_PET_HEART[MUSE_PET_ART_H] = {
    ".#.#.",
    "#o###",
    "#####",
    ".###.",
    "..#..",
};

static const char *const MUSE_PET_CARROT[MUSE_PET_ART_H] = {
    ".o.o.",
    "..o..",
    ".###.",
    ".###.",
    "..#..",
};

static const char *const MUSE_PET_NOTE[MUSE_PET_ART_H] = {
    "..###",
    "..#..",
    "..#..",
    ".##..",
    ".##..",
};

static const char *const MUSE_PET_GIFT[MUSE_PET_ART_H] = {
    "..o..",
    "#####",
    "##o##",
    "#####",
    "##o##",
};
