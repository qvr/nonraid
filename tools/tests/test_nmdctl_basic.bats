#!/usr/bin/env bats
# Unit tests for nmdctl utility functions

setup() {
    export PATH="$BATS_TEST_DIRNAME/..:$PATH"
    source "$BATS_TEST_DIRNAME/../nmdctl"
    # patch out root and module checks
    eval 'check_root() { return 0; }'
    eval 'check_module_loaded() { return 0; }'
}

teardown() {
    # Cleanup any temporary mock files
    rm -f "$BATS_TMPDIR"/mock_nmdstat_*
}

# Create a mock nmdstat file for testing
create_mock_nmdstat() {
    local state=${1:-STOPPED}
    local missing=${2:-0}
    local invalid=${3:-0}
    local resync=${4:-0}
    local resync_action=${5:-check P}
    local resync_pos=${6:-0}
    local resync_size=${7:-0}
    local resync_corr=${8:-0}
    local sync_errs=${9:-0}

    cat << EOF
mdState=$state
mdNumDisks=3
sbName=/test.dat
sbLabel=MockArray
mdNumMissing=$missing
mdNumInvalid=$invalid
mdNumWrong=0
mdNumDisabled=0
mdNumReplaced=0
mdNumNew=0
mdResync=$resync
mdResyncAction=$resync_action
mdResyncCorr=$resync_corr
mdResyncPos=$resync_pos
mdResyncSize=$resync_size
mdResyncDt=10
mdResyncDb=5000
diskSize.0=2000000
diskSize.1=1000000
diskSize.2=1000000
diskSize.29=0
diskId.0=MOCK_PARITY_DISK
diskId.1=MOCK_DATA_DISK_1
diskId.2=MOCK_DATA_DISK_2
diskName.1=nmd1p1
diskName.2=nmd1p2
rdevName.0=sda1
rdevName.1=sdb1
rdevName.2=sdc1
rdevStatus.0=DISK_OK
rdevStatus.1=DISK_OK
rdevStatus.2=DISK_OK
rdevNumErrors.0=0
rdevNumErrors.1=0
rdevNumErrors.2=0
sbSynced=$(( $(date +%s) - 14*24*3600 ))
sbSynced2=$(( $(date +%s) - 14*24*3600 ))
sbSyncErrs=$sync_errs
sbSyncExit=0
EOF
}

@test "nmdctl version check" {
    run "$BATS_TEST_DIRNAME/../nmdctl" --version
    [ "$status" -eq 0 ]
    [[ "$output" =~ nmdctl\ version\ [0-9]+\.[0-9]+\.[0-9]+ ]]
}

@test "nmdctl help command" {
    run "$BATS_TEST_DIRNAME/../nmdctl" --help
    [ "$status" -eq 0 ]
    [[ "$output" =~ "nmdctl - NonRAID array management utility" ]]
    [[ "$output" =~ "Usage:" ]]
}

@test "invalid command handling" {
    run "$BATS_TEST_DIRNAME/../nmdctl" invalid-command
    [ "$status" -eq 1 ]
}

@test "unassign command parameter validation" {
    # Test missing parameter
    run "$BATS_TEST_DIRNAME/../nmdctl" unassign
    [ "$status" -ne 0 ]
    [[ "$output" =~ "Error: Missing slot parameter" ]]

    # Test invalid slot parameter
    run "$BATS_TEST_DIRNAME/../nmdctl" unassign invalid
    [ "$status" -ne 0 ]
    [[ "$output" =~ "Error: Slot must be a number" ]]

    # Test out of range slot
    run "$BATS_TEST_DIRNAME/../nmdctl" unassign 99
    [ "$status" -ne 0 ]
    [[ "$output" =~ "Error: Invalid slot number" ]]
}

@test "convert_blocks_to_gb function" {
    # Test basic conversion (1048576 KB = 1 GB)
    result=$(convert_blocks_to_gb 1048576)
    [ "$result" -eq 1 ]

    # Test with decimals
    result=$(convert_blocks_to_gb 1572864 2)  # 1.5 GB
    [ "$result" = "1.50" ]

    # Test rounding up
    result=$(convert_blocks_to_gb 1048575)
    [ "$result" -eq 1 ]
}

@test "format_time_duration function" {
    # Test seconds
    result=$(format_time_duration 45)
    [ "$result" = "45 sec" ]

    # Test minutes
    result=$(format_time_duration 150)  # 2 minutes 30 seconds
    [ "$result" = "2 minutes, 30 seconds" ]

    # Test hours
    result=$(format_time_duration 7200)  # 2 hours
    [ "$result" = "2 hours, 00 minutes" ]

    # Test days
    result=$(format_time_duration 90000)  # 1 day 1 hour
    [ "$result" = "1 days, 1 hours" ]
}

@test "get_status_visible_length function" {
    # Test known status lengths
    result=$(get_status_visible_length "DISK_OK")
    [ "$result" -eq 2 ]  # "OK"

    result=$(get_status_visible_length "DISK_INVALID")
    [ "$result" -eq 7 ]  # "INVALID"

    result=$(get_status_visible_length "DISK_NP_MISSING")
    [ "$result" -eq 7 ]  # "MISSING"
}

@test "disk size validation" {
    # Test that get_disk_size_kb handles non-existent devices gracefully

    run get_disk_size_kb "/dev/nonexistent"
    [ "$status" -eq 1 ]
}

# Status parsing tests with mock environment
@test "status parsing - HEALTHY state detection" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_healthy"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_healthy"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ Array\ State.*STARTED ]]
    [[ "$output" =~ "Disks Present : 3" ]]
    [[ "$output" =~ Array\ Health.*HEALTHY ]]
}

@test "status parsing - STOPPED state detection" {
    create_mock_nmdstat "STOPPED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_stopped"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_stopped"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ Array\ State.*STOPPED ]]
}

@test "status parsing - DEGRADED state with missing disk" {
    create_mock_nmdstat "STARTED" 1 0 > "$BATS_TMPDIR/mock_nmdstat_degraded"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_degraded"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ Array\ Health.*DEGRADED ]]
}

@test "status parsing - Array size calculation" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_size"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_size"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ Array\ Size.*2\ GB\ \(2\ data\ disk\(s\)\) ]]
}

@test "status parsing - Array with invalid disks" {
    create_mock_nmdstat "STOPPED" 0 1 > "$BATS_TMPDIR/mock_nmdstat_invalid"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_invalid"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ Invalid:\ 1 ]]
    [[ "$output" =~ DEGRADED ]]
}

@test "status parsing - Parity check in progress" {
    # Create mock with parity check in progress (50% complete)
    create_mock_nmdstat "STARTED" 0 0 1 "check P" 500000 1000000 1 0 > "$BATS_TMPDIR/mock_nmdstat_parity_check"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_parity_check"
    run show_status -v

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ Array\ State.*STARTED ]]
    [[ "$output" =~ Array\ Health.*HEALTHY ]]
    [[ "$output" =~ Operation.*Parity-Check\ P ]]
    [[ "$output" =~ MOCK_DATA_DISK_1 ]]
}

@test "status parsing - Parity sync in progress" {
    # Create mock with parity sync in progress
    create_mock_nmdstat "STARTED" 0 1 1 "recon P" 250000 1000000 0 0 > "$BATS_TMPDIR/mock_nmdstat_parity_sync"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_parity_sync"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ Array\ State.*STARTED ]]
    [[ "$output" =~ Array\ Health.*DEGRADED ]]
    [[ "$output" =~ Progress.*25% ]]
    [[ "$output" =~ Operation.*Parity-Sync\ P ]]
    [[ "$output" =~ WARNING:\ Driver\ internal\ state ]]
}

@test "status parsing - Parity check with errors found" {
    # Create mock with parity check that found errors
    create_mock_nmdstat "STARTED" 0 0 0 "check P" 0 0 1 15 > "$BATS_TMPDIR/mock_nmdstat_parity_errors"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_parity_errors"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ Sync\ Errors:\ 15 ]]
}

@test "status parsing - Array with disk errors" {
    # Create mock with disk errors - need to override the basic template
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_disk_errors"
    sed -i -e 's/rdevNumErrors.1=0/rdevNumErrors.1=5/' \
           -e 's/rdevNumErrors.2=0/rdevNumErrors.2=10/' \
           "$BATS_TMPDIR/mock_nmdstat_disk_errors"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_disk_errors"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ 5\ errs ]]
    [[ "$output" =~ 10\ errs ]]
    [[ "$output" =~ WARNING.*15\ total ]]
}

@test "unassigning a disk" {
    create_mock_nmdstat "STOPPED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_stopped"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_stopped"
    run unassign_disk 2 <<< "y"

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ] # fails due to error writing to nmdcmd
    [[ "$output" =~ Unassigning\ disk\ from\ slot\ 2 ]]
}

@test "nmdctl help performance" {
    # Help should complete within reasonable time
    start_time=$(date +%s.%N)
    run timeout 5s "$BATS_TEST_DIRNAME/../nmdctl" --help
    end_time=$(date +%s.%N)

    duration=$(echo "$end_time - $start_time" | bc -l)

    # Should complete in under 1 second
    [ "$(echo "$duration < 1.0" | bc -l)" -eq 1 ]
}

@test "nmdctl version performance" {
    # Version check should be very fast
    start_time=$(date +%s.%N)
    run timeout 2s "$BATS_TEST_DIRNAME/../nmdctl" --version
    end_time=$(date +%s.%N)

    duration=$(echo "$end_time - $start_time" | bc -l)

    # Should complete in under 0.5 seconds
    [ "$(echo "$duration < 0.5" | bc -l)" -eq 1 ]
    [ "$status" -eq 0 ]
}

# Tests for different output formats
@test "status parsing - default format" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_default"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_default"
    run show_status

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "=== NonRAID Array Status ===" ]]
    [[ "$output" =~ Array\ State.*STARTED ]]
    [[ "$output" =~ Array\ Health.*HEALTHY ]]
}

@test "status parsing - default format explicit" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_default_explicit"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_default_explicit"
    run show_status -o default

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "=== NonRAID Array Status ===" ]]
    [[ "$output" =~ Array\ State.*STARTED ]]
    [[ "$output" =~ Array\ Health.*HEALTHY ]]
}

@test "status parsing - prometheus format" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_prometheus"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_prometheus"
    run show_status -o prometheus

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "# HELP nonraid_array_state" ]]
    [[ "$output" =~ "# HELP nonraid_array_health" ]]
    [[ "$output" =~ "nonraid_array_state{label=\"MockArray\"} 1" ]]
    [[ "$output" =~ "nonraid_array_health{label=\"MockArray\",status=\"HEALTHY\"} 0" ]]
}

@test "status parsing - json format" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_json"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_json"
    run show_status -o json

    echo "$status"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" =~ "\"timestamp\":" ]]
    [[ "$output" =~ "\"state\": \"STARTED\"" ]]
    [[ "$output" =~ "\"status\": \"HEALTHY\"" ]]
    [[ "$output" =~ "\"label\": \"MockArray\"" ]]

    # Validate that the output is valid JSON
    echo "$output" | jq . > /dev/null
    [ "$?" -eq 0 ]
}

@test "status parsing - invalid format" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_invalid_format"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_invalid_format"
    run show_status -o invalid

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "Error: Invalid output format" ]]
}

@test "prometheus format - degraded state" {
    create_mock_nmdstat "STARTED" 1 0 > "$BATS_TMPDIR/mock_nmdstat_prometheus_degraded"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_prometheus_degraded"
    run show_status -o prometheus

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "nonraid_array_health{label=\"MockArray\",status=\"DEGRADED\"} 1" ]]
    [[ "$output" =~ "nonraid_nummissing_count{label=\"MockArray\"} 1" ]]
}

@test "json format - degraded state" {
    create_mock_nmdstat "STARTED" 1 0 > "$BATS_TMPDIR/mock_nmdstat_json_degraded"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_json_degraded"
    run show_status -o json

    echo "$status"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" =~ "\"status\": \"DEGRADED\"" ]]
    [[ "$output" =~ "\"code\": 1" ]]
}

@test "data collection functions work correctly" {
    create_mock_nmdstat "STARTED" 0 0 > "$BATS_TMPDIR/mock_nmdstat_collection"

    export PROC_NMDSTAT="$BATS_TMPDIR/mock_nmdstat_collection"

    # Test data collection functions
    get_all_nmdstat_values NMDSTAT_VALUES
    collect_array_summary
    collect_array_health
    collect_array_size_and_parity
    collect_resync_status
    collect_disk_status

    echo "$status"
    echo "$output"

    # Verify ARRAY_STATUS_DATA was populated correctly
    [ "${ARRAY_STATUS_DATA[mdstate]}" = "STARTED" ]
    [ "${ARRAY_STATUS_DATA[sblabel]}" = "MockArray" ]
    [ "${ARRAY_STATUS_DATA[health_status]}" = "HEALTHY" ]
    [ "${ARRAY_STATUS_DATA[health_code]}" = "0" ]
    [ "${ARRAY_STATUS_DATA[data_disk_count]}" = "2" ]
    [ "${ARRAY_STATUS_DATA[has_parity]}" = "true" ]
    [ "${ARRAY_STATUS_DATA[data_size_gb]}" = "2" ]
    [ "${ARRAY_STATUS_DATA[parity_size_gb]}" = "2" ]
    [ -n "${ARRAY_STATUS_DATA[last_sync_timestamp]}" ]
    [ -n "${ARRAY_STATUS_DATA[last_sync_ago]}" ]

    # Verify RESYNC_STATUS_DATA was populated correctly
    [ "${RESYNC_STATUS_DATA[active]}" = "false" ]
    [ "${RESYNC_STATUS_DATA[progress_percent]}" = "0" ]
    [ "${RESYNC_STATUS_DATA[paused]}" = "false" ]
    [ "${RESYNC_STATUS_DATA[pending]}" = "false" ]

    # Verify DISK_STATUS_DATA was populated correctly
    # Check that we have data for parity disk (slot 0)
    [ "${DISK_STATUS_DATA[slot_0_type]}" = "P" ]
    [ "${DISK_STATUS_DATA[slot_0_present]}" = "true" ]
    [ "${DISK_STATUS_DATA[slot_0_size_gb]}" = "2" ]
    [ "${DISK_STATUS_DATA[slot_0_errors]}" = "0" ]

    # Check that we have data for data disks (slots 1 and 2)
    [ "${DISK_STATUS_DATA[slot_1_present]}" = "true" ]
    [ "${DISK_STATUS_DATA[slot_1_type]}" = "data" ]
    [ "${DISK_STATUS_DATA[slot_1_size_gb]}" = "1" ]
    [ "${DISK_STATUS_DATA[slot_1_errors]}" = "0" ]

    [ "${DISK_STATUS_DATA[slot_2_present]}" = "true" ]
    [ "${DISK_STATUS_DATA[slot_2_type]}" = "data" ]
    [ "${DISK_STATUS_DATA[slot_2_size_gb]}" = "1" ]
    [ "${DISK_STATUS_DATA[slot_2_errors]}" = "0" ]
}
