<!---
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

```c++
/*

This file contains some example code for the C++ client. It will
probably be eventually removed in favor of actual runnable examples,
but serves as a guide/docs for the client API design for now.

See class docs for KuduClient, KuduSession, KuduTable for proper docs.
*/

// This is an example of explicit batching done by the client.
// This would be used in contexts like interactive webapps, where
// you are likely going to set a short timeout.
void ExplicitBatchingExample() {
  // Get a reference to the tablet we want to insert into.
  // Note that this may be done without a session, either before or
  // after creating a session, since a session isn't tied to any
  // particular table or set of tables.
  scoped_refptr<KuduTable> t;
  CHECK_OK(client_->OpenTable("my_table", &t));

  // Create a new session. All data-access operations must happen through
  // a session.
  shared_ptr<KuduSession> session(client->NewSession());

  // Setting flush mode to MANUAL_FLUSH makes the session accumulate
  // all operations until the next Flush() call. This is sort of like
  // TCP_CORK.
  CHECK_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  // Insert 100 rows.
  for (int i = 0; i < 100; i++) {
    Insert* ins = t->NewInsert();
    ins->mutable_row()->SetInt64("key", i);
    ins->mutable_row()->SetInt64("val", i * 2);
    // The insert should return immediately after moving the insert
    // into the appropriate buffers. This always returns OK unless the
    // Insert itself is invalid (eg missing a key column).
    CHECK_OK(session->Apply(ins));
  }

  // Update a row.
  gscoped_ptr<Update> upd = t->NewUpdate();
  upd->mutable_row()->SetInt64("key", 1);
  upd->mutable_row()->SetInt64("val", 1 * 2 + 1);

  // Delete a row.
  gscoped_ptr<Delete> del = t->NewDelete();
  del->mutable_row()->SetInt64("key", 2); // only specify key.

  // Setting a timeout on the session applies to the next Flush call.
  session->SetTimeoutMillis(300);

  // After accumulating all of the stuff in the batch, call Flush()
  // to send the updates in one go. This may be done either sync or async.
  // Sync API example:
  {
    // Returns an Error if any insert in the batch had an issue.
    CHECK_OK(session->Flush());
    // Call session->GetPendingErrors() to get errors.
  }

  // Async API example:
  {
    // Returns immediately, calls Callback when either success or failure.
    CHECK_OK(session->FlushAsync(MyCallback));
    // TBD: should you be able to use the same session before the Callback has
    // been called? Or require that you do nothing with this session while
    // in-flight (which is more like what JDBC does I think)
  }
}

// This is an example of how a "bulk ingest" program might work -- one in
// which the client just wants to shove a bunch of data in, and perhaps
// fail if it ever gets an error.
void BulkIngestExample() {
  scoped_refptr<KuduTable> t;
  CHECK_OK(client_->OpenTable("my_table", &t));
  shared_ptr<KuduSession> session(client->NewSession());

  // If the amount of buffered data in RAM is larger than this amount,
  // blocks the writer from performing more inserts until memory has
  // been freed (either by inserts succeeding or timing out).
  session->SetBufferSpace(32 * 1024 * 1024);

  // Set a long timeout for this kind of usecase. This determines how long
  // Flush() may block for, as well as how long Apply() may block due to
  // the buffer being full.
  session->SetTimeoutMillis(60 * 1000);

  // In AUTO_FLUSH_BACKGROUND mode, the session will try to accumulate batches
  // for optimal efficiency, rather than flushing each operation.
  CHECK_OK(session->SetFlushMode(KuduSession::AUTO_FLUSH_BACKGROUND));

  for (int i = 0; i < 10000; i++) {
    gscoped_ptr<Insertion> ins = t->NewInsertion();
    ins->SetInt64("key", i);
    ins->SetInt64("val", i * 2);
    // This will start getting written in the background.
    // If there are any pending errors, it will return a bad Status,
    // and the user should call GetPendingErrors()
    // This may block if the buffer is full.
    CHECK_OK(session->Apply(&ins));
    if (session->HasErrors())) {
      LOG(FATAL) << "Failed to insert some rows: " << DumpErrors(session);
    }
  }
  // Blocks until remaining buffered operations have been flushed.
  // May also use the async API per above.
  Status s = session->Flush());
  if (!s.ok()) {
    LOG(FATAL) << "Failed to insert some rows: " << DumpErrors(session);
  }
}
```
