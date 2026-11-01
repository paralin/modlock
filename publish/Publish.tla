---- MODULE Publish ----
\* archive.go Archive finishes/closes its .tmp zip before rename.
\* github.go GitHub creates a draft, uploads, then publishes it. Replacing
\* a public asset retains --clobber: replaceGitHubAsset resolves/downloads the
\* exact old asset before upload, attempts restoration on failure using a
\* context independent of publish cancellation, and keeps the copy if that
\* also fails. A release with a failed restore is explicitly repairable,
\* rather than complete. It reports the copy's path. Existing public releases
\* are never hidden to make replacement appear atomic.
\* Callers: modlock publish archive/github. Hyperline.Publish sends one packed
\* request; the service's World transaction is in Hyperline, outside this
\* client model. This models command/network failure and cancellation as an
\* upload failure. Local filesystem crashes and simultaneous publishers of
\* the same tag are outside it; GitHub offers no atomic asset replacement.
CONSTANTS ArchiveReady, DraftFirst, KeepBackup
VARIABLE s
Init == s = [archive |-> "none", temp |-> "none", github |-> "none",
             asset |-> FALSE, backup |-> FALSE, operation |-> "new"]
Pack == /\ s.temp = "none" /\ s' = [s EXCEPT !.temp = "writing"]
Finish == /\ s.temp = "writing" /\ s' = [s EXCEPT !.temp = "complete"]
Rename == /\ s.temp = "complete" \/ (s.temp = "writing" /\ ~ArchiveReady)
          /\ s' = [s EXCEPT !.archive = s.temp, !.temp = "none"]
ArchiveFail == /\ s.temp = "writing" /\ s' = [s EXCEPT !.temp = "failed"]
Create == /\ s.archive = "complete" /\ s.operation = "new"
          /\ s' = [s EXCEPT !.github = IF DraftFirst THEN "draft" ELSE "public",
                            !.operation = "upload"]
Upload == /\ s.operation = "upload"
          /\ s' = [s EXCEPT !.asset = TRUE,
                !.operation = IF s.github = "draft" THEN "publish" ELSE "done",
                !.backup = FALSE]
UploadFail == /\ s.operation = "upload"
              /\ s' = [s EXCEPT !.operation = IF s.backup THEN "restore" ELSE "error"]
Publish == /\ s.operation = "publish" /\ s.asset
           /\ s' = [s EXCEPT !.github = "public", !.operation = "done"]
\* A retry on an existing draft can upload and publish it.
RetryDraft == /\ s.operation = "error" /\ s.github = "draft"
              /\ s' = [s EXCEPT !.operation = "upload"]
\* An existing complete public release may be replaced. The recovery copy
\* precedes clobber, which deletes its old asset even if the upload then fails.
Replace == /\ s.operation = "done" /\ s.github = "public"
           /\ s' = [s EXCEPT !.backup = KeepBackup, !.asset = FALSE, !.operation = "upload"]
Restore == /\ s.operation = "restore"
           /\ s' = [s EXCEPT !.asset = TRUE, !.backup = FALSE, !.operation = "error"]
RestoreFail == /\ s.operation = "restore"
               /\ s' = [s EXCEPT !.operation = "error"]
Next == Pack \/ Finish \/ Rename \/ ArchiveFail \/ Create \/ Upload \/ UploadFail
        \/ Publish \/ RetryDraft \/ Replace \/ Restore \/ RestoreFail \/ UNCHANGED s
Spec == Init /\ [][Next]_s
ArchiveAtomic == s.archive \in {"none", "complete"}
PublicRecoverable == s.github = "public" => s.asset \/ s.backup
PublishComplete == s.operation = "done" => s.github = "public" /\ s.asset
====
