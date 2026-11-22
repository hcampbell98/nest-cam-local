/*
 * Synchronizes aria-invalid, dynamic aria-describedby error links, and a single
 * polite live-region announcer with CSS :user-invalid so errors appear only
 * after user interaction (or a submit attempt).
 */

type ValidatableField = HTMLInputElement | HTMLSelectElement | HTMLTextAreaElement;

export function attachAccessibleValidation(root: ParentNode): () => void {
  const announcer = document.createElement('div');
  announcer.setAttribute('aria-live', 'polite');
  announcer.className = 'visually-hidden';
  document.body.append(announcer);

  let announceTimer: number | undefined;
  let announcedField: ValidatableField | undefined;
  const flagged = new WeakSet<ValidatableField>();
  const announced = new WeakSet<ValidatableField>();
  const dirtyFallback = new WeakMap<
    ValidatableField,
    { interacted: boolean; blurred: boolean }
  >();
  const supportsUserInvalid =
    typeof CSS !== 'undefined' && CSS.supports('selector(:user-invalid)');

  const linkError = (el: ValidatableField, shown: boolean) => {
    const errorId = el.dataset.error;
    if (!errorId) return;
    const ids = (el.getAttribute('aria-describedby') || '')
      .split(/\s+/)
      .filter((id) => id && id !== errorId);
    if (shown) ids.push(errorId);
    if (ids.length) el.setAttribute('aria-describedby', ids.join(' '));
    else el.removeAttribute('aria-describedby');
  };

  const spokenText = (node: Element): string => {
    const copy = node.cloneNode(true) as Element;
    copy.querySelectorAll('[aria-hidden="true"]').forEach((n) => n.remove());
    copy.querySelectorAll('[role="img"][aria-label]').forEach((n) => {
      n.replaceWith(`${n.getAttribute('aria-label')}: `);
    });
    return (copy.textContent || '').replace(/\s+/g, ' ').trim();
  };

  const announceError = (el: ValidatableField) => {
    const errorId = el.dataset.error;
    if (!errorId) return;
    const errorEl = document.getElementById(errorId);
    if (!errorEl) return;
    const labelEl = el.labels?.[0];
    const label = labelEl ? `${spokenText(labelEl)}: ` : '';
    window.clearTimeout(announceTimer);
    announcer.textContent = '';
    announcedField = el;
    announceTimer = window.setTimeout(() => {
      announcer.textContent = label + spokenText(errorEl);
    }, 500);
  };

  const isFieldInvalid = (el: ValidatableField): boolean => {
    if (supportsUserInvalid) {
      return el.matches(':user-invalid');
    }
    const st = dirtyFallback.get(el);
    if (!st || !st.interacted || !st.blurred) return false;
    return !el.validity.valid;
  };

  const syncAria = (el: EventTarget | null, shouldAnnounce = false) => {
    if (
      !(
        el instanceof HTMLInputElement ||
        el instanceof HTMLSelectElement ||
        el instanceof HTMLTextAreaElement
      )
    ) {
      return;
    }

    const invalid = isFieldInvalid(el);
    if (!supportsUserInvalid) {
      const st = dirtyFallback.get(el);
      const engaged = Boolean(st?.interacted && st?.blurred);
      el.classList.toggle('user-invalid-fallback', engaged && !el.validity.valid);
      el.classList.toggle('user-valid-fallback', engaged && el.validity.valid);
    }

    if (invalid) {
      if (shouldAnnounce && !announced.has(el)) {
        announceError(el);
        announced.add(el);
      }
      el.setAttribute('aria-invalid', 'true');
      linkError(el, true);
      flagged.add(el);
    } else if (flagged.has(el) || el.getAttribute('aria-invalid') !== 'true') {
      el.setAttribute('aria-invalid', 'false');
      linkError(el, false);
      flagged.delete(el);
      announced.delete(el);
      if (announcedField === el) {
        window.clearTimeout(announceTimer);
        announcer.textContent = '';
      }
    }
  };

  root.querySelectorAll('input, select, textarea').forEach((el) => syncAria(el));

  const onBlur = (e: Event) => {
    const t = e.target;
    if (
      t instanceof HTMLInputElement ||
      t instanceof HTMLSelectElement ||
      t instanceof HTMLTextAreaElement
    ) {
      const st = dirtyFallback.get(t) ?? { interacted: false, blurred: false };
      st.blurred = true;
      dirtyFallback.set(t, st);
    }
    syncAria(t, true);
  };

  const onInput = (e: Event) => {
    const t = e.target;
    if (
      t instanceof HTMLInputElement ||
      t instanceof HTMLSelectElement ||
      t instanceof HTMLTextAreaElement
    ) {
      const st = dirtyFallback.get(t) ?? { interacted: false, blurred: false };
      st.interacted = true;
      dirtyFallback.set(t, st);
      if (t.hasAttribute('aria-invalid')) syncAria(t);
    }
  };

  const onChange = (e: Event) => {
    const t = e.target;
    if (
      t instanceof HTMLInputElement ||
      t instanceof HTMLSelectElement ||
      t instanceof HTMLTextAreaElement
    ) {
      dirtyFallback.set(t, { interacted: true, blurred: true });
    }
    syncAria(t, true);
  };

  const onInvalid = (e: Event) => {
    const t = e.target;
    if (
      t instanceof HTMLInputElement ||
      t instanceof HTMLSelectElement ||
      t instanceof HTMLTextAreaElement
    ) {
      dirtyFallback.set(t, { interacted: true, blurred: true });
      announced.add(t);
      syncAria(t);
    }
  };

  root.addEventListener('blur', onBlur, true);
  root.addEventListener('input', onInput);
  root.addEventListener('change', onChange, true);
  root.addEventListener('invalid', onInvalid, true);

  return () => {
    root.removeEventListener('blur', onBlur, true);
    root.removeEventListener('input', onInput);
    root.removeEventListener('change', onChange, true);
    root.removeEventListener('invalid', onInvalid, true);
    window.clearTimeout(announceTimer);
    announcer.remove();
  };
}
