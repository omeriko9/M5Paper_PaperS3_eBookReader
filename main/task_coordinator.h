#pragma once
/**
 * TaskCoordinator - Priority-based task coordination for ESP32
 * 
 * This module provides a mechanism for coordinating multiple FreeRTOS tasks
 * that need to access shared resources, with the following guarantees:
 * 
 * 1. NO DEADLOCKS - Uses atomic flags and short mutex holds, never blocks indefinitely
 * 2. NO WDT TRIGGERS - All waits have timeouts and yield properly
 * 3. PRIORITY RESPECT - Higher priority operations preempt lower priority ones
 * 
 * Architecture:
 * - Tasks are assigned priorities (CRITICAL > HIGH > NORMAL > LOW > IDLE)
 * - Before doing work, tasks check if they should yield to higher priority tasks
 * - Long operations are broken into small chunks with yield points
 * - Mutex holds are kept extremely short (< 1ms) - only for data structure updates
 * - File I/O is NEVER done while holding a mutex
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>
#include <functional>

// Task priority levels - higher number = higher priority
enum class TaskPriority : int {
    IDLE = 0,       // Background indexing when system is completely idle
    LOW = 1,        // Background metrics calculation, scanning
    NORMAL = 2,     // Pre-rendering next/prev pages
    HIGH = 3,       // Active user interaction
    CRITICAL = 4    // Book opening, immediate UI response needed
};

class TaskCoordinator {
public:
    static TaskCoordinator& getInstance() {
        static TaskCoordinator instance;
        return instance;
    }
    
    /**
     * Signal that a high-priority operation is starting.
     * Lower priority tasks should pause immediately.
     */
    void requestHighPriority(TaskPriority priority) {
        int prio = static_cast<int>(priority);
        int current = highestRequestedPriority.load();
        while (prio > current) {
            if (highestRequestedPriority.compare_exchange_weak(current, prio)) {
                break;
            }
        }
        
        // Also set the specific flags for quick checking
        if (priority >= TaskPriority::CRITICAL) {
            criticalPending.store(true);
        } else if (priority >= TaskPriority::HIGH) {
            highPriorityPending.store(true);
        }
    }
    
    /**
     * Signal that a high-priority operation has completed.
     */
    void releaseHighPriority(TaskPriority priority) {
        int prio = static_cast<int>(priority);
        int current = highestRequestedPriority.load();
        if (current == prio) {
            highestRequestedPriority.store(0);
        }
        
        if (priority >= TaskPriority::CRITICAL) {
            criticalPending.store(false);
        } else if (priority >= TaskPriority::HIGH) {
            highPriorityPending.store(false);
        }
    }
    
    /**
     * Check if a task at the given priority should yield.
     * Returns true if a higher priority task is waiting.
     */
    bool shouldYield(TaskPriority myPriority) const {
        int myPrio = static_cast<int>(myPriority);
        return highestRequestedPriority.load() > myPrio;
    }
    
    /**
     * Quick check if any critical operation is pending.
     */
    bool isCriticalPending() const {
        return criticalPending.load();
    }
    
    /**
     * Quick check if any high-priority operation is pending.
     */
    bool isHighPriorityPending() const {
        return highPriorityPending.load() || criticalPending.load();
    }
    
    /**
     * Yield if a higher priority task is waiting.
     * Returns true if we yielded (caller might want to abort or checkpoint).
     * 
     * @param myPriority The priority of the calling task
     * @param maxWaitMs Maximum time to wait before resuming (0 = yield once)
     * @return true if we yielded, false if no yield was needed
     */
    bool yieldIfNeeded(TaskPriority myPriority, uint32_t maxWaitMs = 100) {
        if (!shouldYield(myPriority)) {
            return false;
        }
        
        uint32_t waited = 0;
        while (shouldYield(myPriority) && waited < maxWaitMs) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
            wdtReset();
        }
        return true;
    }
    
    /**
     * Execute a function in small chunks, yielding between chunks if needed.
     * This is the preferred way to do long-running background work.
     * 
     * @param myPriority Priority of this operation
     * @param totalItems Total number of items to process
     * @param chunkSize Number of items per chunk
     * @param processItem Function to process each item (receives item index)
     * @param onYield Optional callback when yielding (for cleanup/checkpoint)
     * @return Number of items processed (may be less than total if aborted)
     */
    size_t processInChunks(
        TaskPriority myPriority,
        size_t totalItems,
        size_t chunkSize,
        std::function<bool(size_t)> processItem,
        std::function<void()> onYield = nullptr
    ) {
        size_t processed = 0;
        
        while (processed < totalItems) {
            // Check if we should yield before starting a chunk
            if (shouldYield(myPriority)) {
                if (onYield) onYield();
                yieldIfNeeded(myPriority, 500);
                
                // After yielding, check if we should abort
                if (shouldYield(myPriority)) {
                    return processed;  // Abort - higher priority is still waiting
                }
            }
            
            // Process a chunk
            size_t chunkEnd = std::min(processed + chunkSize, totalItems);
            while (processed < chunkEnd) {
                wdtReset();
                if (!processItem(processed)) {
                    return processed;  // Item processing requested abort
                }
                processed++;
            }
            
            // Brief yield between chunks to keep system responsive
            vTaskDelay(1);
        }
        
        return processed;
    }
    
    /**
     * RAII helper for high-priority sections
     */
    class PriorityGuard {
    public:
        PriorityGuard(TaskPriority priority) : priority(priority) {
            TaskCoordinator::getInstance().requestHighPriority(priority);
        }
        ~PriorityGuard() {
            TaskCoordinator::getInstance().releaseHighPriority(priority);
        }
    private:
        TaskPriority priority;
    };

private:
    TaskCoordinator() = default;
    ~TaskCoordinator() = default;
    TaskCoordinator(const TaskCoordinator&) = delete;
    TaskCoordinator& operator=(const TaskCoordinator&) = delete;
    
    void wdtReset() {
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }
    
    std::atomic<int> highestRequestedPriority{0};
    std::atomic<bool> criticalPending{false};
    std::atomic<bool> highPriorityPending{false};
};

// Convenience macro for priority sections
#define PRIORITY_SECTION(priority) \
    TaskCoordinator::PriorityGuard _priorityGuard##__LINE__(priority)
