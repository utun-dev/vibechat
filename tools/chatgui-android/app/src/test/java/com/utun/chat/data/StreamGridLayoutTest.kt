package com.utun.chat.data

import org.junit.Assert.*
import org.junit.Test

class StreamGridLayoutTest {
    @Test fun portraitAndLandscapeUseDifferentShapes() {
        assertEquals(StreamGridShape(1, 2), streamGridShape(List(2) { 16.0 / 9 }, 400.0, 800.0))
        assertEquals(StreamGridShape(2, 1), streamGridShape(List(2) { 16.0 / 9 }, 1600.0, 600.0))
        assertEquals(StreamGridShape(1, 4), streamGridShape(List(4) { 16.0 / 9 }, 400.0, 800.0))
    }
    @Test fun portraitSourcesUseColumnsWhereLandscapeSourcesUseRows() {
        assertEquals(StreamGridShape(1, 2), streamGridShape(List(2) { 16.0 / 9 }, 400.0, 600.0))
        assertEquals(StreamGridShape(2, 1), streamGridShape(List(2) { 9.0 / 16 }, 400.0, 600.0))
    }
    @Test fun incompleteLastRowUsesTheFullWidth() {
        val shape = streamGridShape(List(3) { 16.0 / 9 }, 1600.0, 900.0)
        assertEquals(StreamGridShape(2, 2), shape)
        assertEquals(2, shape.columnsInRow(0, 3)); assertEquals(1, shape.columnsInRow(1, 3))
    }
    @Test fun oneSourceAlwaysOwnsTheWholeViewport() {
        for (aspect in listOf(16.0 / 9, 9.0 / 16, 1.0)) {
            val shape = streamGridShape(listOf(aspect), 400.0, 800.0)
            assertEquals(StreamGridShape(1, 1), shape)
            assertEquals(1, shape.columnsInRow(0, 1))
        }
    }
    @Test fun everySourceHasACellAcrossCountsAndSizes() {
        for (count in 1..40) for ((width, height) in listOf(400.0 to 800.0, 800.0 to 400.0, 900.0 to 900.0)) {
            val shape = streamGridShape(List(count) { 16.0 / 9 }, width, height)
            assertTrue(shape.columns > 0 && shape.rows > 0)
            assertTrue(shape.columns * shape.rows >= count)
            assertEquals((count + shape.columns - 1) / shape.columns, shape.rows)
        }
    }
    @Test fun emptyAndZeroSizedLayoutIsSafe() {
        assertEquals(StreamGridShape(1, 1), streamGridShape(emptyList(), 400.0, 800.0))
        assertEquals(StreamGridShape(1, 1), streamGridShape(List(4) { 16.0 / 9 }, 0.0, 800.0))
    }
}
