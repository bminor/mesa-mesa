def write_template(out_file, template, environment):
    try:
        with open(out_file, "w", encoding="utf-8") as f:
            f.write(template.render(**environment))
    except Exception:
        # In the event there's an error, this imports some helpers from mako
        # to print a useful stack trace and prints it, then exits with
        # status 1, if python is run with debug; otherwise it just raises
        # the exception
        import sys
        from mako import exceptions

        print(exceptions.text_error_template().render(), file=sys.stderr)
        sys.exit(1)
