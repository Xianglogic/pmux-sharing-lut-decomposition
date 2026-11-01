STATUS=ERROR
sby -f pmux_case04_feedback.data_out.sby > /dev/null && STATUS=$(awk '{print $1}' pmux_case04_feedback.data_out/status)
echo $STATUS > status
case $STATUS in
    PASS)
        echo "Proved equivalence of partition 'pmux_case04_feedback.data_out' using strategy 'pdr'"
    ;;
    FAIL)
        echo "Could not prove equivalence of partition 'pmux_case04_feedback.data_out' using strategy 'pdr': partitions not equivalent"
    ;;
    UNKNOWN)
        echo "Could not prove equivalence of partition 'pmux_case04_feedback.data_out' using strategy 'pdr': equivalence unknown"
    ;;
    TIMEOUT)
        echo "Could not prove equivalence of partition 'pmux_case04_feedback.data_out' using strategy 'pdr': timeout"
    ;;
    *)
        cat pmux_case04_feedback.data_out/ERROR 2> /dev/null
        echo "Execution of strategy 'pdr' on partition 'pmux_case04_feedback.data_out' encountered an error."
        echo "More details can be found in '/tmp/pmux_nopost_sc3wldoo/proofs_zero_init/pmux_case04_feedback_baseline_rtl_boundary/run/strategies/pmux_case04_feedback.data_out/pdr/pmux_case04_feedback.data_out/logfile.txt'."
        exit 1
    ;;
esac
exit 0

