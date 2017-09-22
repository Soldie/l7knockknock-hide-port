# knock-ssh

Hide an ssh service behind another tcp port. For example behind a https server (port 443). Only when you receive a certain piece of text (or bytes) will the ssh port be unlocked.

A simplified alternative to port knocking, very handy for cases where your on restricted internet with only a few open ports.
