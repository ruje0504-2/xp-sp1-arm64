
/* Keep the original demand notification after dispatcher queue insertion. */
VOID ExQueueWorkItem(PWORK_QUEUE_ITEM item, WORK_QUEUE_TYPE type)
{
    if (!item || !item->WorkerRoutine || (ULONG)type >= MaximumWorkQueue ||
        KeGetCurrentIrql() > DISPATCH_LEVEL || item->List.Flink ||
        ExWorkerQueue[type].WorkerQueue.Header.Type != QueueObject ||
        ExWorkerQueue[type].Info.QueueDisabled)
        KeBugCheckEx(WORKER_INVALID, (ULONG_PTR)item, type, 0, 0);

    PEX_WORK_QUEUE queue = &ExWorkerQueue[type];
    KeInsertQueue(&queue->WorkerQueue, &item->List);
    if (ExpNewThreadNecessary(queue))
        KeSetEvent(&ExpThreadSetManagerEvent, 0, FALSE);
}
