use tracing::field::{Field, Visit};
use tracing_subscriber::field::MakeVisitor;
use tracing_subscriber::fmt::format::Writer;
use tracing_subscriber::fmt::{FormatFields, FormattedFields};
use tracing_subscriber::EnvFilter;

/// Field names whose values must be replaced with `***` in log output.
const REDACTED_FIELDS: &[&str] = &["password", "token", "pat", "credentials"];

fn is_redacted(field: &Field) -> bool {
    REDACTED_FIELDS.contains(&field.name())
}

// ---------------------------------------------------------------------------
// RedactingVisitor — rewrites sensitive field values.
// ---------------------------------------------------------------------------

pub struct RedactingVisitor<'a> {
    writer: Writer<'a>,
    is_first: bool,
}

impl<'a> RedactingVisitor<'a> {
    fn new(writer: Writer<'a>) -> Self {
        Self {
            writer,
            is_first: true,
        }
    }

    fn write_field_name(&mut self, field: &Field) {
        if !self.is_first {
            let _ = write!(self.writer, " ");
        }
        let _ = write!(self.writer, "{}=", field.name());
        self.is_first = false;
    }
}

impl Visit for RedactingVisitor<'_> {
    fn record_str(&mut self, field: &Field, value: &str) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value:?}");
        }
    }

    fn record_debug(&mut self, field: &Field, value: &dyn std::fmt::Debug) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value:?}");
        }
    }

    fn record_i64(&mut self, field: &Field, value: i64) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value}");
        }
    }

    fn record_u64(&mut self, field: &Field, value: u64) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value}");
        }
    }

    fn record_f64(&mut self, field: &Field, value: f64) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value}");
        }
    }

    fn record_bool(&mut self, field: &Field, value: bool) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value}");
        }
    }

    fn record_error(&mut self, field: &Field, value: &(dyn std::error::Error + 'static)) {
        self.write_field_name(field);
        if is_redacted(field) {
            let _ = write!(self.writer, "***");
        } else {
            let _ = write!(self.writer, "{value}");
        }
    }
}

// ---------------------------------------------------------------------------
// RedactingFields — FormatFields impl so tracing-subscriber uses our visitor.
// ---------------------------------------------------------------------------

#[derive(Clone, Default)]
pub struct RedactingFields;

impl<'writer> MakeVisitor<Writer<'writer>> for RedactingFields {
    type Visitor = RedactingVisitor<'writer>;

    fn make_visitor(&self, target: Writer<'writer>) -> Self::Visitor {
        RedactingVisitor::new(target)
    }
}

impl<'writer> FormatFields<'writer> for RedactingFields {
    fn format_fields<R: tracing_subscriber::field::RecordFields>(
        &self,
        writer: Writer<'writer>,
        fields: R,
    ) -> std::fmt::Result {
        let mut visitor = self.make_visitor(writer);
        fields.record(&mut visitor);
        Ok(())
    }

    fn add_fields(
        &self,
        current: &'writer mut FormattedFields<Self>,
        fields: &tracing::span::Record<'_>,
    ) -> std::fmt::Result {
        let writer = current.as_writer();
        let mut visitor = self.make_visitor(writer);
        fields.record(&mut visitor);
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Public initialiser — used by all three binary crates.
// ---------------------------------------------------------------------------

/// Initialise `tracing-subscriber` with:
/// - `RUST_LOG` env-filter (default INFO when unset).
/// - Secret field redaction for `password`, `token`, `pat`, `credentials`.
///
/// Call once at the start of `main`. The subscriber is set as the global default.
///
/// # Panics
/// Panics if a global subscriber has already been set. Safe to call once per process.
pub fn init_tracing() {
    use tracing_subscriber::prelude::*;

    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));

    let fmt_layer = tracing_subscriber::fmt::layer()
        .fmt_fields(RedactingFields)
        .with_writer(std::io::stderr);

    tracing_subscriber::registry()
        .with(filter)
        .with(fmt_layer)
        .init();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use tracing_subscriber::fmt::MakeWriter;

    /// A writer that captures bytes into a shared `Vec<u8>` for inspection.
    #[derive(Clone)]
    struct CapturingWriter(Arc<Mutex<Vec<u8>>>);

    impl CapturingWriter {
        fn new() -> (Self, Arc<Mutex<Vec<u8>>>) {
            let buf = Arc::new(Mutex::new(Vec::new()));
            (Self(buf.clone()), buf)
        }
    }

    impl std::io::Write for CapturingWriter {
        fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(buf);
            Ok(buf.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    impl<'a> MakeWriter<'a> for CapturingWriter {
        type Writer = CapturingWriter;

        fn make_writer(&'a self) -> Self::Writer {
            self.clone()
        }
    }

    #[test]
    fn tracing_event_redacts_password_field() {
        use tracing_subscriber::prelude::*;

        let (writer, buf) = CapturingWriter::new();

        let subscriber = tracing_subscriber::registry().with(
            tracing_subscriber::fmt::layer()
                .fmt_fields(RedactingFields)
                .with_writer(writer),
        );

        tracing::subscriber::with_default(subscriber, || {
            tracing::info!(password = "secret123", user = "alice", "login attempt");
        });

        let output = String::from_utf8(buf.lock().unwrap().clone()).expect("utf-8 output");

        assert!(
            !output.contains("secret123"),
            "raw secret must not appear in log output; got: {output}"
        );
        assert!(
            output.contains("***"),
            "redaction marker *** must appear in log output; got: {output}"
        );
    }
}
