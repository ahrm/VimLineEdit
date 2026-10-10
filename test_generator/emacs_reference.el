;;; emacs_reference.el --- Replay editing fixtures in real Emacs -*- lexical-binding: t; -*-

(require 'json)

(defun reference-event (event)
  "Convert a portable input event to an Emacs keyboard event."
  (let* ((key (alist-get 'key event))
         (modifiers (mapcar #'intern (alist-get 'modifiers event)))
         (special (cdr (assoc key '(("RET" . 13) ("TAB" . 9) ("DEL" . 127)
                                    ("LEFT" . left) ("RIGHT" . right)
                                    ("UP" . up) ("DOWN" . down)
                                    ("HOME" . home) ("END" . end)
                                    ("DELETE" . delete))))))
    (event-convert-list (append modifiers (list (or special (string-to-char key)))))))

(defun reference-undo-boundary ()
  ;; Keep typed text together; give other commands normal undo boundaries.
  (unless (and (eq this-command 'self-insert-command)
               (eq last-command 'self-insert-command))
    (undo-boundary)))

(defun reference-run ()
  (let* ((json-object-type 'alist)
         (json-array-type 'list)
         (events (json-read-file (getenv "EMACS_FIXTURE_INPUT")))
         (keys (mapcar #'reference-event events)))
    (switch-to-buffer (generate-new-buffer " *editing-reference*"))
    (fundamental-mode)
    (transient-mark-mode 1)
    (electric-indent-mode -1)
    (setq-local indent-tabs-mode nil)
    (setq-local tab-width 8)
    (setq-local require-final-newline nil)
    (setq select-enable-clipboard nil select-enable-primary nil
          interprogram-cut-function nil interprogram-paste-function nil
          kill-ring nil kill-ring-yank-pointer nil)
    (buffer-enable-undo)
    (setq buffer-undo-list nil)
    (add-hook 'pre-command-hook #'reference-undo-boundary nil t)
    ;; Resume only when a command really quits. Splitting at every C-g also
    ;; exits a failing isearch, where C-g should merely remove failed input.
    (let ((remaining (vconcat keys)) (consumed 0))
      (add-hook 'pre-command-hook
                (lambda () (setq consumed executing-kbd-macro-index)) nil t)
      (while (> (length remaining) 0)
        (condition-case nil
            (progn (execute-kbd-macro remaining) (setq remaining []))
          (quit
           (unless (> consumed 0) (error "Quit without consuming input"))
           (setq remaining (substring remaining consumed) consumed 0)))))
    (when (or (active-minibuffer-window)
              (bound-and-true-p isearch-mode)
              (not (equal (buffer-name) " *editing-reference*")))
      (error "Fixture left the editing buffer or an unfinished search"))
    (let ((coding-system-for-write 'utf-8-unix))
      (write-region (point-min) (point-max) (getenv "EMACS_FIXTURE_OUTPUT") nil 'silent))))

(condition-case err
    (reference-run)
  (error (princ (format "Reference replay failed: %s\n" (error-message-string err)))
         (kill-emacs 1)))
