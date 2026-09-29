/*
 * P4-LMS - UI jobs: the one way background tasks change the screen
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * Only the LVGL task may touch widgets.  Background tasks (status polling,
 * commands, the SOURCE browser, Wi-Fi) used to grab the display lock to do
 * it themselves; if LVGL was busy drawing they either waited or gave up
 * ("Failed to acquire LVGL lock").  Now they hand the LVGL task a job:
 *
 *   ui_post(fn, &data, sizeof data)  - copy the data, queue the job, carry on.
 *                                      For frequent updates (track info, art,
 *                                      Wi-Fi label...).  Never blocks.
 *   ui_call(fn, &ctx)                - queue the job and wait until it has run.
 *                                      For step-by-step flows (SOURCE lists,
 *                                      Wi-Fi / server screens); ctx may point
 *                                      at the caller's local variables.
 *
 * Jobs run in order, between frames, from an LVGL timer (see ui_jobs_start).
 */
typedef void (*ui_job_fn_t)(void *arg);

/* Short notifications above the navigation bar (18b_navigation.c):
 * ui_toast() on the LVGL task, ui_toast_post() from any task. */
static void ui_toast(const char *text);
static void ui_toast_post(const char *text);

typedef struct {
    ui_job_fn_t  fn;
    void        *arg;
    bool         owned;       /* arg is a heap copy to free after running */
    TaskHandle_t waiter;      /* task to wake when done (ui_call), or NULL */
    volatile bool *done;      /* set once the job has run (ui_call)        */
} ui_job_t;

#define UI_JOB_QUEUE_LEN   32
#define UI_JOBS_PER_TICK   24

static QueueHandle_t ui_job_queue = NULL;
static TaskHandle_t  ui_task_handle = NULL;       /* the LVGL task, once known */
static uint32_t      ui_jobs_dropped = 0;

static void ui_jobs_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (ui_task_handle == NULL) {
        ui_task_handle = xTaskGetCurrentTaskHandle();
    }
    ui_job_t job;
    for (int n = 0; n < UI_JOBS_PER_TICK && xQueueReceive(ui_job_queue, &job, 0) == pdTRUE; n++) {
        job.fn(job.arg);
        if (job.owned) heap_caps_free(job.arg);
        if (job.done != NULL) *job.done = true;
        if (job.waiter != NULL) xTaskNotifyGive(job.waiter);
    }
}

/* Create the queue and the timer that runs the jobs.  Call once, with the
 * display lock held, before any background task starts. */
static void ui_jobs_start(void)
{
    ui_job_queue = xQueueCreate(UI_JOB_QUEUE_LEN, sizeof(ui_job_t));
    ESP_ERROR_CHECK(ui_job_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    lv_timer_create(ui_jobs_timer_cb, 10, NULL);
}

static bool ui_in_ui_task(void)
{
    return ui_task_handle != NULL && xTaskGetCurrentTaskHandle() == ui_task_handle;
}

/* Queue fn(copy of data) and return straight away.  Returns false (and the
 * update is simply skipped) if the queue is full; frequent updates repeat
 * a moment later anyway. */
static bool ui_post(ui_job_fn_t fn, const void *data, size_t len)
{
    if (ui_job_queue == NULL || fn == NULL) return false;

    ui_job_t job = { .fn = fn, .arg = NULL, .owned = false, .waiter = NULL, .done = NULL };
    if (len > 0) {
        job.arg = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (job.arg == NULL) return false;
        memcpy(job.arg, data, len);
        job.owned = true;
    }
    if (xQueueSend(ui_job_queue, &job, 0) != pdTRUE) {
        if (job.owned) heap_caps_free(job.arg);
        if ((ui_jobs_dropped++ % 50) == 0) {
            ESP_LOGW(TAG, "UI job queue full - skipped an update (%u so far)",
                     (unsigned)ui_jobs_dropped);
        }
        return false;
    }
    return true;
}

/* Run fn(ctx) on the LVGL task and wait for it to finish.  Safe to call from
 * the LVGL task itself (it then just runs fn directly). */
static void ui_call(ui_job_fn_t fn, void *ctx)
{
    if (fn == NULL) return;
    if (ui_job_queue == NULL || ui_in_ui_task()) {
        fn(ctx);
        return;
    }
    volatile bool done = false;
    ui_job_t job = { .fn = fn, .arg = ctx, .owned = false,
                     .waiter = xTaskGetCurrentTaskHandle(), .done = &done };
    /* The caller's ctx lives on its stack, so wait for the job no matter how
     * long LVGL is busy; the queue always drains.  The done flag guards
     * against a wake-up meant for something else ending the wait early. */
    while (xQueueSend(ui_job_queue, &job, pdMS_TO_TICKS(1000)) != pdTRUE) { }
    while (!done) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}
