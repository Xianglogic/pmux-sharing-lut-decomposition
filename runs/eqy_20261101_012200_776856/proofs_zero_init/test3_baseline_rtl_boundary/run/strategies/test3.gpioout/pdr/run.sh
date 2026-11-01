STATUS=ERROR
sby -f test3.gpioout.sby > /dev/null && STATUS=$(awk '{print $1}' test3.gpioout/status)
echo $STATUS > status
case $STATUS in
    PASS)
        echo "Proved equivalence of partition 'test3.gpioout' using strategy 'pdr'"
    ;;
    FAIL)
        echo "Could not prove equivalence of partition 'test3.gpioout' using strategy 'pdr': partitions not equivalent"
    ;;
    UNKNOWN)
        echo "Could not prove equivalence of partition 'test3.gpioout' using strategy 'pdr': equivalence unknown"
    ;;
    TIMEOUT)
        echo "Could not prove equivalence of partition 'test3.gpioout' using strategy 'pdr': timeout"
    ;;
    *)
        cat test3.gpioout/ERROR 2> /dev/null
        echo "Execution of strategy 'pdr' on partition 'test3.gpioout' encountered an error."
        echo "More details can be found in '/tmp/pmux_nopost_sc3wldoo/proofs_zero_init/test3_baseline_rtl_boundary/run/strategies/test3.gpioout/pdr/test3.gpioout/logfile.txt'."
        exit 1
    ;;
esac
exit 0

